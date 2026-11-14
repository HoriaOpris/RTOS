/*
 * Driver for the 6-character segment LCD on the MSP-EXP430FR6989 LaunchPad.
 *
 * The display shows digits, letters (as capitals) and a few symbols.
 * Characters it cannot draw are shown as a blank.
 *
 * Usage:
 *   - Call lcd_init() once, before the scheduler starts.
 *   - Call the other functions from tasks only, never from an interrupt:
 *     they take a mutex so that two tasks cannot mix their output.
 */
#ifndef LCD_H
#define LCD_H

#include <stdint.h>

#define LCD_NUM_CHARS   6

/* Set up the LCD controller and clear the display. */
void lcd_init( void );

/* Blank the whole display. */
void lcd_clear( void );

/* Show one character. position 0 is the leftmost of the six; positions
 * outside 0..5 are ignored. */
void lcd_put_char( uint8_t position, char c );

/* Show a string from the left edge. Only the first six characters fit;
 * shorter strings are padded with blanks. */
void lcd_print( const char *text );

/* Show a number against the right edge. Numbers above 999999 show their
 * last six digits. */
void lcd_print_uint( uint32_t value );

#endif /* LCD_H */
