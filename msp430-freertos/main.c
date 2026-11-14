/*
 * FreeRTOS on the MSP-EXP430FR6989 LaunchPad.
 *
 * Data flow:
 *
 *   Mac --USB--> UART RX interrupt --queue--> console task --mutex--> UART TX
 *                                                  |
 *   blink task (red LED, 1 Hz) --shared counters---+--> green LED
 *
 * Serial port: 115200 baud, 8N1, on the LaunchPad's "application UART".
 * The console task draws a status screen that refreshes twice a second and
 * accepts the commands on, off, stats and lcd (type, then Enter).
 * The board's own LCD is driven through lcd.h.
 */
#include <msp430.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include "lcd.h"

/* --- Board pins --------------------------------------------------------- */
#define LED_RED_BIT     BIT0    /* P1.0 */
#define LED_GREEN_BIT   BIT7    /* P9.7 */
#define UART_TX_BIT     BIT4    /* P3.4, eUSCI_A1 */
#define UART_RX_BIT     BIT5    /* P3.5, eUSCI_A1 */

#define RX_QUEUE_LENGTH 32
#define LINE_MAX        40
#define RESULT_MAX      70
#define REFRESH_MS      500

/* Terminal control sequences (understood by screen and most terminals). */
#define TERM_HOME       "\x1b[H"        /* cursor to top left */
#define TERM_EOL        "\x1b[K\r\n"    /* clear rest of line, next line */
#define TERM_CLEAR_DOWN "\x1b[J"        /* clear from cursor to end of screen */
#define TERM_PROMPT_ROW "\x1b[15;1H"    /* cursor to the input line */

/* The FreeRTOS heap. PERSISTENT places it in FRAM rather than the 2 KB RAM. */
#pragma PERSISTENT( ucHeap )
uint8_t ucHeap[ configTOTAL_HEAP_SIZE ] = { 0 };

static QueueHandle_t xRxQueue;          /* bytes: UART interrupt -> console task */
static SemaphoreHandle_t xUartMutex;    /* guards the UART transmitter */
static TaskHandle_t xBlinkTask, xConsoleTask;

static volatile uint16_t usRxDropped;   /* bytes lost because the queue was full */

/* Written by the blink task, read by the console task.
 *
 * usFlashCount is 16 bits, which this 16-bit CPU reads in one instruction, so
 * it needs no protection. ulHalfSeconds is 32 bits: reading it takes two
 * instructions, and a task switch between them could return a torn value, so
 * every access is wrapped in a critical section. */
static volatile uint16_t usFlashCount;
static volatile uint32_t ulHalfSeconds;

/* --- UART output -------------------------------------------------------- */

/* Send a string. The mutex stops two tasks interleaving their output. */
static void uart_puts( const char *s )
{
    xSemaphoreTake( xUartMutex, portMAX_DELAY );
    while( *s != '\0' )
    {
        while( ( UCA1IFG & UCTXIFG ) == 0 ) {}
        UCA1TXBUF = ( uint8_t ) *s++;
    }
    xSemaphoreGive( xUartMutex );
}

/* Convert a number to decimal text, at least minDigits wide (zero padded).
 * Returns a pointer into buf, which must hold at least 11 characters. */
static char *uint_to_text( uint32_t value, uint16_t minDigits, char *buf )
{
    char *p = &buf[ 10 ];
    uint16_t digits = 0;

    *p = '\0';
    do
    {
        *--p = ( char ) ( '0' + ( value % 10 ) );
        value /= 10;
        digits++;
    } while( ( value != 0 ) || ( digits < minDigits ) );

    return p;
}

static void uart_put_uint( uint32_t value, uint16_t minDigits )
{
    char buf[ 11 ];

    uart_puts( uint_to_text( value, minDigits, buf ) );
}

/* --- UART input --------------------------------------------------------- */

/* Receive interrupt: keep it short, hand the byte to a task, never block. */
#pragma vector = USCI_A1_VECTOR
__interrupt void uart_rx_isr( void )
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    uint8_t c;

    if( ( UCA1IFG & UCRXIFG ) != 0 )
    {
        c = ( uint8_t ) UCA1RXBUF;      /* reading clears the interrupt flag */

        if( xQueueSendFromISR( xRxQueue, &c, &xHigherPriorityTaskWoken ) != pdPASS )
        {
            usRxDropped++;
        }
    }

    /* Wake the CPU if it was sleeping, and switch straight to the console
     * task if the byte unblocked it. */
    __bic_SR_register_on_exit( SCG1 + SCG0 + OSCOFF + CPUOFF );
    portYIELD_FROM_ISR( xHigherPriorityTaskWoken );
}

/* --- Blink task --------------------------------------------------------- */

static void blink_task( void *pvParameters )
{
    TickType_t xLastWake = xTaskGetTickCount();

    ( void ) pvParameters;

    for( ;; )
    {
        P1OUT ^= LED_RED_BIT;
        if( ( P1OUT & LED_RED_BIT ) != 0 )
        {
            usFlashCount++;
        }

        vTaskDelayUntil( &xLastWake, pdMS_TO_TICKS( 500 ) );

        taskENTER_CRITICAL();
        ulHalfSeconds++;
        taskEXIT_CRITICAL();
    }
}

/* --- Console task ------------------------------------------------------- */

static char cLine[ LINE_MAX + 1 ];      /* command being typed */
static uint16_t usLineLen;
static char cResult[ RESULT_MAX + 1 ] = "-";    /* outcome of the last command */

static void result_set( const char *s )
{
    strncpy( cResult, s, RESULT_MAX );
    cResult[ RESULT_MAX ] = '\0';
}

static void result_append( const char *s )
{
    strncat( cResult, s, RESULT_MAX - strlen( cResult ) );
}

static void result_append_uint( uint32_t value )
{
    char buf[ 11 ];

    result_append( uint_to_text( value, 1, buf ) );
}

static void handle_line( const char *line )
{
    if( strcmp( line, "on" ) == 0 )
    {
        P9OUT |= LED_GREEN_BIT;
        result_set( "green LED turned on" );
    }
    else if( strcmp( line, "off" ) == 0 )
    {
        P9OUT &= ~LED_GREEN_BIT;
        result_set( "green LED turned off" );
    }
    else if( strcmp( line, "stats" ) == 0 )
    {
        result_set( "heap free " );
        result_append_uint( xPortGetFreeHeapSize() );
        result_append( " B, stack free blink " );
        result_append_uint( uxTaskGetStackHighWaterMark( xBlinkTask ) );
        result_append( " console " );
        result_append_uint( uxTaskGetStackHighWaterMark( xConsoleTask ) );
        result_append( " words, rx lost " );
        result_append_uint( usRxDropped );
    }
    else if( strncmp( line, "lcd ", 4 ) == 0 )
    {
        lcd_print( &line[ 4 ] );
        result_set( "LCD shows: " );
        result_append( &line[ 4 ] );
    }
    else
    {
        result_set( "echo: " );
        result_append( line );
    }
}

/* Redraw only the input line; used after each keystroke. */
static void draw_prompt( void )
{
    uart_puts( TERM_PROMPT_ROW "> " );
    uart_puts( cLine );
    uart_puts( "\x1b[K" );
}

/* Redraw the whole screen in place. Each line clears its own tail, so the
 * screen never blanks and does not flicker. */
static void draw_screen( void )
{
    uint32_t ulSeconds;

    taskENTER_CRITICAL();
    ulSeconds = ulHalfSeconds / 2;
    taskEXIT_CRITICAL();

    uart_puts( TERM_HOME "FreeRTOS on MSP430FR6989 LaunchPad" TERM_EOL TERM_EOL );

    uart_puts( "Red LED flashes : " );
    uart_put_uint( usFlashCount, 1 );
    uart_puts( TERM_EOL );

    uart_puts( "Up time         : " );
    uart_put_uint( ulSeconds / 3600, 1 );
    uart_puts( ":" );
    uart_put_uint( ( ulSeconds / 60 ) % 60, 2 );
    uart_puts( ":" );
    uart_put_uint( ulSeconds % 60, 2 );
    uart_puts( TERM_EOL );

    uart_puts( "Green LED       : " );
    uart_puts( ( ( P9OUT & LED_GREEN_BIT ) != 0 ) ? "on" : "off" );
    uart_puts( TERM_EOL );

    uart_puts( "Last result     : " );
    uart_puts( cResult );
    uart_puts( TERM_EOL TERM_EOL );

    uart_puts( "Type a command, then press Enter:" TERM_EOL
               "  on     turn the green LED on" TERM_EOL
               "  off    turn the green LED off" TERM_EOL
               "  stats  show heap and stack usage" TERM_EOL
               "  lcd X  show X on the board's LCD (6 characters)" TERM_EOL
               "  (anything else is echoed back)" TERM_EOL
               TERM_EOL );

    /* The input line is last, so the cursor is left where the user types. */
    uart_puts( "> " );
    uart_puts( cLine );
    uart_puts( TERM_CLEAR_DOWN );
}

/* Blocks on the queue with a timeout: a key press wakes it at once, and the
 * timeout gives the periodic refresh. It uses no CPU in between. */
static void console_task( void *pvParameters )
{
    uint8_t c;

    ( void ) pvParameters;

    lcd_print( "RTOS" );

    for( ;; )
    {
        if( xQueueReceive( xRxQueue, &c, pdMS_TO_TICKS( REFRESH_MS ) ) != pdPASS )
        {
            draw_screen();              /* timed out: periodic refresh */
        }
        else if( ( c == '\r' ) || ( c == '\n' ) )
        {
            if( usLineLen > 0 )
            {
                handle_line( cLine );
                usLineLen = 0;
                cLine[ 0 ] = '\0';
            }
            draw_screen();
        }
        else if( ( c == 0x08 ) || ( c == 0x7f ) )   /* backspace or delete */
        {
            if( usLineLen > 0 )
            {
                cLine[ --usLineLen ] = '\0';
            }
            draw_prompt();
        }
        else if( ( c >= ' ' ) && ( c < 0x7f ) && ( usLineLen < LINE_MAX ) )
        {
            cLine[ usLineLen++ ] = ( char ) c;
            cLine[ usLineLen ] = '\0';
            draw_prompt();
        }
    }
}

/* --- Hardware setup ----------------------------------------------------- */

static void hardware_init( void )
{
    WDTCTL = WDTPW | WDTHOLD;

    /* LEDs off, as outputs. */
    P1OUT &= ~LED_RED_BIT;
    P1DIR |= LED_RED_BIT;
    P9OUT &= ~LED_GREEN_BIT;
    P9DIR |= LED_GREEN_BIT;

    /* UART pins to eUSCI_A1. */
    P3SEL0 |= UART_TX_BIT | UART_RX_BIT;
    P3SEL1 &= ~( UART_TX_BIT | UART_RX_BIT );

    /* Release the pins from their power-on high-impedance state. */
    PM5CTL0 &= ~LOCKLPM5;

    /* Clocks: 8 MHz internal oscillator for the CPU and peripherals. */
    CSCTL0_H = CSKEY_H;
    CSCTL1 = DCOFSEL_6;
    CSCTL2 = SELA__VLOCLK | SELS__DCOCLK | SELM__DCOCLK;
    CSCTL3 = DIVA__1 | DIVS__1 | DIVM__1;
    CSCTL0_H = 0;

    /* UART: 115200 baud from 8 MHz (values from the TI user's guide table). */
    UCA1CTLW0 = UCSWRST;
    UCA1CTLW0 |= UCSSEL__SMCLK;
    UCA1BRW = 4;
    UCA1MCTLW = UCOS16 | UCBRF_5 | 0x5500;
    UCA1CTLW0 &= ~UCSWRST;
    UCA1IE |= UCRXIE;
}

/* Called by the FreeRTOS port to start the tick: Timer0_A, 1 MHz, 1 ms period. */
void vApplicationSetupTimerInterrupt( void )
{
    TA0CTL = 0;
    TA0CCR0 = ( uint16_t ) ( ( configCPU_CLOCK_HZ / 8UL ) / configTICK_RATE_HZ ) - 1U;
    TA0CCTL0 = CCIE;
    TA0CTL = TASSEL__SMCLK | ID__8 | MC__UP | TACLR;
}

/* --- FreeRTOS hooks ----------------------------------------------------- */

/* Runs whenever no task is ready: sleep until the next interrupt. */
void vApplicationIdleHook( void )
{
    __bis_SR_register( LPM0_bits | GIE );
    __no_operation();
}

/* Both error hooks stop with the two LEDs lit so a fault is visible. */
static void fatal_error( void )
{
    taskDISABLE_INTERRUPTS();
    P1OUT |= LED_RED_BIT;
    P9OUT |= LED_GREEN_BIT;
    for( ;; ) {}
}

void vApplicationMallocFailedHook( void )
{
    fatal_error();
}

void vApplicationStackOverflowHook( TaskHandle_t xTask, char *pcTaskName )
{
    ( void ) xTask;
    ( void ) pcTaskName;
    fatal_error();
}

/* Stop the watchdog before C start-up initialises variables. */
int _system_pre_init( void )
{
    WDTCTL = WDTPW | WDTHOLD;
    return 1;
}

/* --- Entry point -------------------------------------------------------- */

int main( void )
{
    hardware_init();
    lcd_init();

    xRxQueue = xQueueCreate( RX_QUEUE_LENGTH, sizeof( uint8_t ) );
    xUartMutex = xSemaphoreCreateMutex();
    configASSERT( xRxQueue != NULL );
    configASSERT( xUartMutex != NULL );

    /* The console task has the higher priority so input is handled promptly. */
    xTaskCreate( blink_task, "blink", configMINIMAL_STACK_SIZE, NULL, 1, &xBlinkTask );
    xTaskCreate( console_task, "console", 160, NULL, 2, &xConsoleTask );

    vTaskStartScheduler();

    /* Only reached if the idle task could not be created. */
    fatal_error();
    return 0;
}
