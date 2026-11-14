/*
 * Driver for the 6-character segment LCD on the MSP-EXP430FR6989 LaunchPad.
 * See lcd.h for the API.
 *
 * How the display works: each character is made of 14 segments. The LCD_C
 * controller in the chip has a small block of memory (LCDM1, LCDM2, ...)
 * where each bit switches one segment on or off, so drawing a character
 * means writing two bytes of segment bits to the right place.
 */
#include <msp430.h>

#include "FreeRTOS.h"
#include "semphr.h"

#include "lcd.h"

/* Where each character's two bytes live, as an index into LCDMEM, from the
 * leftmost character to the rightmost. The order is irregular because of how
 * the glass is wired to the chip (taken from TI's LaunchPad demo code). */
static const uint8_t ucPosition[ LCD_NUM_CHARS ] = { 9, 5, 3, 18, 14, 7 };

/* Segment bits for each glyph: { first byte, second byte }.
 * First byte:  0x80 top, 0x40 upper right, 0x20 lower right, 0x10 bottom,
 *              0x08 lower left, 0x04 upper left, 0x02 and 0x01 the two halves
 *              of the middle bar.
 * Second byte: the diagonal and vertical centre segments.
 * Digit and letter values are from TI's LaunchPad demo code. */
static const uint8_t ucDigit[ 10 ][ 2 ] =
{
    { 0xFC, 0x28 }, { 0x60, 0x20 }, { 0xDB, 0x00 }, { 0xF3, 0x00 }, { 0x67, 0x00 },
    { 0xB7, 0x00 }, { 0xBF, 0x00 }, { 0xE4, 0x00 }, { 0xFF, 0x00 }, { 0xF7, 0x00 }
};

static const uint8_t ucLetter[ 26 ][ 2 ] =
{
    { 0xEF, 0x00 }, /* A */ { 0xF1, 0x50 }, /* B */ { 0x9C, 0x00 }, /* C */
    { 0xF0, 0x50 }, /* D */ { 0x9F, 0x00 }, /* E */ { 0x8F, 0x00 }, /* F */
    { 0xBD, 0x00 }, /* G */ { 0x6F, 0x00 }, /* H */ { 0x90, 0x50 }, /* I */
    { 0x78, 0x00 }, /* J */ { 0x0E, 0x22 }, /* K */ { 0x1C, 0x00 }, /* L */
    { 0x6C, 0xA0 }, /* M */ { 0x6C, 0x82 }, /* N */ { 0xFC, 0x00 }, /* O */
    { 0xCF, 0x00 }, /* P */ { 0xFC, 0x02 }, /* Q */ { 0xCF, 0x02 }, /* R */
    { 0xB7, 0x00 }, /* S */ { 0x80, 0x50 }, /* T */ { 0x7C, 0x00 }, /* U */
    { 0x0C, 0x28 }, /* V */ { 0x6C, 0x0A }, /* W */ { 0x00, 0xAA }, /* X */
    { 0x00, 0xB0 }, /* Y */ { 0x90, 0x28 }  /* Z */
};

static SemaphoreHandle_t xLcdMutex;

/* Write one character's segment bits. The caller must hold the mutex. */
static void write_char( uint8_t position, char c )
{
    uint8_t first = 0x00, second = 0x00;    /* blank unless matched below */
    uint8_t index;

    if( position >= LCD_NUM_CHARS )
    {
        return;
    }

    if( ( c >= 'a' ) && ( c <= 'z' ) )
    {
        c = ( char ) ( c - 'a' + 'A' );
    }

    if( ( c >= '0' ) && ( c <= '9' ) )
    {
        first = ucDigit[ c - '0' ][ 0 ];
        second = ucDigit[ c - '0' ][ 1 ];
    }
    else if( ( c >= 'A' ) && ( c <= 'Z' ) )
    {
        first = ucLetter[ c - 'A' ][ 0 ];
        second = ucLetter[ c - 'A' ][ 1 ];
    }
    else if( c == '-' )
    {
        first = 0x03;                       /* middle bar */
    }
    else if( c == '_' )
    {
        first = 0x10;                       /* bottom bar */
    }
    else if( c == '+' )
    {
        first = 0x03;                       /* middle bar and centre vertical */
        second = 0x50;
    }

    index = ucPosition[ position ];
    LCDMEM[ index ] = ( char ) first;
    LCDMEM[ index + 1 ] = ( char ) second;
}

void lcd_init( void )
{
    /* Hand the LCD pins (segments 0-21 and 26-43) to the LCD controller. */
    LCDCPCTL0 = 0xFFFF;
    LCDCPCTL1 = 0xFC3F;
    LCDCPCTL2 = 0x0FFF;

    /* Clocked from ACLK, 4-mux glass, low-power waveform. main.c runs ACLK
     * from the internal ~9.4 kHz oscillator, so divide by 4 for a refresh of
     * roughly 300 Hz. */
    LCDCCTL0 = LCDDIV__1 | LCDPRE__4 | LCD4MUX | LCDLP;

    /* Generate the LCD voltage inside the chip with the charge pump. */
    LCDCVCTL = VLCD_1 | VLCDREF_0 | LCDCPEN;
    LCDCCPCTL = LCDCPCLKSYNC;

    LCDCMEMCTL = LCDCLRM;                   /* clear every segment */
    LCDCCTL0 |= LCDON;

    xLcdMutex = xSemaphoreCreateMutex();
    configASSERT( xLcdMutex != NULL );
}

void lcd_clear( void )
{
    lcd_print( "" );
}

void lcd_put_char( uint8_t position, char c )
{
    xSemaphoreTake( xLcdMutex, portMAX_DELAY );
    write_char( position, c );
    xSemaphoreGive( xLcdMutex );
}

void lcd_print( const char *text )
{
    uint8_t position;

    xSemaphoreTake( xLcdMutex, portMAX_DELAY );
    for( position = 0; position < LCD_NUM_CHARS; position++ )
    {
        if( *text != '\0' )
        {
            write_char( position, *text++ );
        }
        else
        {
            write_char( position, ' ' );
        }
    }
    xSemaphoreGive( xLcdMutex );
}

void lcd_print_uint( uint32_t value )
{
    char text[ LCD_NUM_CHARS + 1 ];
    int16_t i;

    /* Fill from the right; leading positions stay blank. */
    text[ LCD_NUM_CHARS ] = '\0';
    for( i = LCD_NUM_CHARS - 1; i >= 0; i-- )
    {
        if( ( value != 0 ) || ( i == LCD_NUM_CHARS - 1 ) )
        {
            text[ i ] = ( char ) ( '0' + ( value % 10 ) );
            value /= 10;
        }
        else
        {
            text[ i ] = ' ';
        }
    }

    lcd_print( text );
}
