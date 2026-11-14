/*
 * FreeRTOS on the MSP-EXP430FR6989 LaunchPad.
 *
 * Data flow:
 *
 *   Mac --USB--> UART RX interrupt --queue--> command task --mutex--> UART TX
 *                                                  |
 *   blink task (red LED, 1 Hz)                     +--> green LED
 *
 * Serial port: 115200 baud, 8N1, on the LaunchPad's "application UART".
 * Commands (end each with Enter): on, off, stats. Anything else is echoed.
 */
#include <msp430.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/* --- Board pins --------------------------------------------------------- */
#define LED_RED_BIT     BIT0    /* P1.0 */
#define LED_GREEN_BIT   BIT7    /* P9.7 */
#define UART_TX_BIT     BIT4    /* P3.4, eUSCI_A1 */
#define UART_RX_BIT     BIT5    /* P3.5, eUSCI_A1 */

#define RX_QUEUE_LENGTH 32
#define LINE_MAX        40

/* The FreeRTOS heap. PERSISTENT places it in FRAM rather than the 2 KB RAM. */
#pragma PERSISTENT( ucHeap )
uint8_t ucHeap[ configTOTAL_HEAP_SIZE ] = { 0 };

static QueueHandle_t xRxQueue;          /* bytes: UART interrupt -> command task */
static SemaphoreHandle_t xUartMutex;    /* guards the UART transmitter */

static volatile uint16_t usRxDropped;   /* bytes lost because the queue was full */

/* --- UART --------------------------------------------------------------- */

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

static void uart_put_uint( const char *label, uint16_t value )
{
    char buf[ 6 ];
    char *p = &buf[ 5 ];

    *p = '\0';
    do
    {
        *--p = ( char ) ( '0' + ( value % 10 ) );
        value /= 10;
    } while( value != 0 );

    uart_puts( label );
    uart_puts( p );
    uart_puts( "\r\n" );
}

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

    /* Wake the CPU if it was sleeping, and switch straight to the command
     * task if the byte unblocked it. */
    __bic_SR_register_on_exit( SCG1 + SCG0 + OSCOFF + CPUOFF );
    portYIELD_FROM_ISR( xHigherPriorityTaskWoken );
}

/* --- Tasks -------------------------------------------------------------- */

static TaskHandle_t xBlinkTask, xCommandTask;

static void blink_task( void *pvParameters )
{
    TickType_t xLastWake = xTaskGetTickCount();

    ( void ) pvParameters;

    for( ;; )
    {
        P1OUT ^= LED_RED_BIT;
        vTaskDelayUntil( &xLastWake, pdMS_TO_TICKS( 500 ) );
    }
}

static void print_stats( void )
{
    uart_put_uint( "uptime ticks (wraps at 65536): ", xTaskGetTickCount() );
    uart_put_uint( "heap free bytes: ", ( uint16_t ) xPortGetFreeHeapSize() );
    uart_put_uint( "blink stack free words: ", uxTaskGetStackHighWaterMark( xBlinkTask ) );
    uart_put_uint( "command stack free words: ", uxTaskGetStackHighWaterMark( xCommandTask ) );
    uart_put_uint( "rx bytes dropped: ", usRxDropped );
}

static void handle_line( const char *line )
{
    if( strcmp( line, "on" ) == 0 )
    {
        P9OUT |= LED_GREEN_BIT;
        uart_puts( "green LED on\r\n" );
    }
    else if( strcmp( line, "off" ) == 0 )
    {
        P9OUT &= ~LED_GREEN_BIT;
        uart_puts( "green LED off\r\n" );
    }
    else if( strcmp( line, "stats" ) == 0 )
    {
        print_stats();
    }
    else
    {
        uart_puts( "echo: " );
        uart_puts( line );
        uart_puts( "\r\n" );
    }
}

/* Blocks on the queue, so it uses no CPU until a byte arrives. */
static void command_task( void *pvParameters )
{
    static char line[ LINE_MAX + 1 ];
    uint16_t len = 0;
    uint8_t c;

    ( void ) pvParameters;

    uart_puts( "\r\nFreeRTOS on MSP430FR6989 ready. Commands: on, off, stats\r\n" );

    for( ;; )
    {
        xQueueReceive( xRxQueue, &c, portMAX_DELAY );

        if( ( c == '\r' ) || ( c == '\n' ) )
        {
            if( len > 0 )
            {
                line[ len ] = '\0';
                handle_line( line );
                len = 0;
            }
        }
        else if( len < LINE_MAX )
        {
            line[ len++ ] = ( char ) c;
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

    xRxQueue = xQueueCreate( RX_QUEUE_LENGTH, sizeof( uint8_t ) );
    xUartMutex = xSemaphoreCreateMutex();
    configASSERT( xRxQueue != NULL );
    configASSERT( xUartMutex != NULL );

    /* The command task has the higher priority so input is handled promptly. */
    xTaskCreate( blink_task, "blink", configMINIMAL_STACK_SIZE, NULL, 1, &xBlinkTask );
    xTaskCreate( command_task, "command", 160, NULL, 2, &xCommandTask );

    vTaskStartScheduler();

    /* Only reached if the idle task could not be created. */
    fatal_error();
    return 0;
}
