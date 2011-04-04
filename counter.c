#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <util/delay.h>
#include <avr/sleep.h>

/*
  ATtiny4313
*/
#define T1 5
#define MOSI 5
#define MISO 6
#define SCK 7

/*
 * Table which controls the divider ratio, it is adapted to the change in part
 * from 744040 to 744020 without changing the wiring.
 * 744020 would have the mask and shift equal each other.
 */

const struct _prescaler {
    const uint8_t mask;
    const uint8_t shift;
} divider_ctrl[8] = {
    {0x0, 1}, // Qa up to 20Mhz (2Hz step)
    {0x1, 2}, // Qb up to 40Mhz (4Hz step)
    {0x4, 3}, // Qc up to 80Mhz (8Hz step)
    {0x2, 4}, // Qd up to 160Mhz (16Hz step)
    {0x3, 7}, // Qg up to 1280Mhz (128Hz step)
    {0x5, 8}, // Qh
    {0x6, 9}, // Qi
    {0x7, 10} // Qj
};

#define DIVIDER_PORT PORTB
#define DIVIDER_MASK (~0x7)
uint8_t divider_id; // selected prescaler mode

/*
 * Maximum sampling frequency is 10Mhz
 */

#define PRECISION double
#define COUNTER_INPUT T1

volatile uint8_t measuring; // measurement in progress

PRECISION last_frequency[4]; // last measured frequencies
uint8_t last_frequency_counter = 0;

PRECISION frequency; // moving average frequency

char freq_buffer[14]; // displayed frequency in text form (max 999.999999Mhz)
char presc_buffer[5] = ">   "; // displayed prescaler in text form
#define OCR0A_START 250 // second stage timebase divider
#define F_MEASURING ((PRECISION)256*OCR0A_START*timer0E_start) // number of cycles we are using to measure frequency

#define F_CPU_REAL F_CPU*0.998 // real cpu speed
#define CALIBRATION ((PRECISION)F_CPU_REAL/F_MEASURING) // timer0 ratio

/*
 * Inductance metering
 */
#define CAPACITOR 1e-9 // 1nF high precision reference capacitor
PRECISION inductor = 102.3e-6; // default inductance value
uint8_t calibrationMode = 1;

/*
 * LCD configuration
 */
#define LCD PORTD
#define RS 4
#define EN 6

/* Increment counter to allow measuring high frequencies */
volatile uint16_t timer1E;

/*
 * buttons
 */
#define BUTTONA (PINB & _BV(PB4))
#define BUTTONB (PINB & _BV(PB3))

ISR(TIMER1_OVF_vect)
{
    timer1E++;
}

/* Decrement counter to enable measuring smaller frequencies */
volatile uint16_t timer0E;
uint16_t timer0E_start;

/* Capture variables to ensure presicion even when using logic in interrupt */
volatile uint16_t timer1L;
volatile uint16_t timer1H;

ISR(TIMER0_COMPA_vect)
{
    timer1L = TCNT1;
    timer1H = timer1E;

    if(timer0E){
        timer0E--;
        if(!timer0E){
            TCCR1B = 0; // Stop Timer1 counter
            TCCR0B = 0; // Stop Timer0 counter
            measuring = 0;
        }
    }
}

void setDivider(uint8_t divider)
{
    divider_id = divider;
    DIVIDER_PORT &= DIVIDER_MASK;
    DIVIDER_PORT |= divider_ctrl[divider_id].mask;
}

/*
  setup measurement
  time is the binary shift applied to the second:
  0 - full second
  1 - half the second
  2 - quarter
  3 - 1/8
*/
void startMeasurement(uint8_t time_shift)
{
    timer0E = timer0E_start >> time_shift;
    timer1E = 0;
    TCNT1 = 0;
    TCNT0 = 0;
    measuring = 1;

    cli();
    TCCR1B = _BV(CS10) | _BV(CS11) | _BV(CS12); // T1 on rising edge
    TCCR0B = _BV(CS02); // prescaler 256
    sei();
}

void lcd_put(uint8_t data)
{
    LCD |= _BV(EN); // set EN high
    _delay_us(100);

    LCD = (LCD & 0x70) + (data >> 4); // write high nibble

    PIND |= _BV(EN); // flip EN low
    _delay_us(100);

    LCD |= _BV(EN); // set EN high
    _delay_us(100);

    LCD = (LCD & 0x70) + (data & 0x0f); // write low nibble

    PIND |= _BV(EN); // strobe EN
    _delay_us(100);
}

#define LCD_COMMAND 0
#define LCD_DATA 1

void lcd_mode(uint8_t mode)
{
    if(mode) LCD |= _BV(RS); // RS high to data mode
    else LCD &= ~_BV(RS); // RS low to command mode
}

void lcd_command8(uint8_t data)
{
    LCD &= ~_BV(RS); // RS low to command mode
    LCD |= _BV(EN); // set EN high
    _delay_us(100);

    LCD = (LCD & 0x70) + (data >> 4); // write high nibble

    PIND |= _BV(EN); // flip EN low
    _delay_us(100);
}

void lcd_init(void)
{
    _delay_ms(20);

    lcd_command8(0x30); //init 5ms delay
    _delay_ms(5);

    lcd_command8(0x30); //init 100us delay
    _delay_us(100);

    lcd_command8(0x30); //init 39us delay
    _delay_us(45);

    lcd_command8(0x20); //preliminary 4bit mode
    _delay_us(45);

    lcd_mode(LCD_COMMAND);
    lcd_put(0x28); //4bit mode, 2 lines 39us delay
    _delay_us(45);

    lcd_put(0x08); //display off
    _delay_us(45);

    lcd_put(0x0C); //display off
    _delay_us(45);

    lcd_put(0x01); //clear
    _delay_us(2000);

    lcd_put(0x06); //write mode (shift cursor and keep display)
    _delay_us(39);
}

void lcd_clear(void)
{
    lcd_mode(LCD_COMMAND);
    lcd_put(0x01); //2000us delay
    _delay_us(2000);
}

void lcd_line(uint8_t line)
{
    lcd_mode(LCD_COMMAND);
    lcd_put(0x80+line*0x40);
    _delay_us(45);
}

void lcd_write(const char* str)
{
    lcd_mode(LCD_DATA);
    while(*str){
        lcd_put(*str);
        _delay_us(50);
        str++;
    }
}

int main(void)
{
    /*
      PORTD controls the LCD, but pin T1 is an external clock input for the counter
    */
    DDRD = ~_BV(COUNTER_INPUT);
    PORTD = 0;

    /*
      PORTB controls the divider and gets input from buttons
      it is also reserved for serial communication
    */
    DDRB = _BV(PB0) | _BV(PB1) | _BV(PB2) | _BV(MOSI) | _BV(SCK);

    /* enable the button pull ups and set external divider to 0 shift */
    PORTB = _BV(PB3) | _BV(PB4);

    /*
      Set Timer1 to count external pulses (rising edge) with no prescaler
      - also enable overflow interrupt
      - timer stopped by default
    */
    TIMSK |= _BV(TOIE1);
    TCCR1B = 0;
    TCCR1A = 0;

    /*
      Set Timer0 to act as timebase
      - enable compare interrupt A to stop counting pulses
      - set prescaler to 256
      - mode to TOP = OCR0A
      - timer stopped by default
    */
    TIMSK |= _BV(OCIE0A);
    TCCR0A = _BV(WGM01);
    TCCR0B = 0;
    OCR0A = OCR0A_START; // 312,5 interrupts per second
    timer0E_start = 312; // measuring time 19968000 cycles eg. 0.9984sec

    /*
      Initialize LCD
    */
    lcd_init();
    
    while(1){
        set_sleep_mode(SLEEP_MODE_IDLE);
        sleep_enable();

        /*
          Get rough frequency read and adjust prescaler to get the best accuracy
          Use shorter measuring time
        */

        setDivider(4); // set divider to about 1280Mhz maximum
        startMeasurement(3); // 1/8 sec

        while(measuring){
            sleep_cpu();
        }

        // set better prescaler
        if(timer1H==0 && timer1L<15625) setDivider(0); // cca 18 Mhz
        else if(timer1H==0 && timer1L<37000) setDivider(1); // cca 38 Mhz
        else if(timer1H<=1 && timer1L<10635) setDivider(2); // cca 78 Mhz
        else if(timer1H<=2 && timer1L<22000) setDivider(3); // cca 156.7 Mhz
        
        /*
          Measure again with better prescaler
        */
        startMeasurement(0);

        while(measuring){
            sleep_cpu();
        }

        /*
          Compute the frequency with all possible precision
        */

        last_frequency[last_frequency_counter] =
            (uint16_t)_BV(divider_ctrl[divider_id].shift) // prescaler
            *((65536*timer1H)+timer1L) // timer1 counter
            *CALIBRATION;
        last_frequency_counter = (last_frequency_counter + 1) & 0x3; // aka modulo 4

        /*
         * Prepare moving average
         */
        frequency = round((last_frequency[0] +
                           last_frequency[1] +
                           last_frequency[2] +
                           last_frequency[3]) / 4);

        lcd_clear();
        lcd_mode(LCD_DATA);        

        /* LC measuring */
        if(!BUTTONA || !BUTTONB){
            PRECISION divider = (!BUTTONA)?CAPACITOR:inductor;
            frequency = pow(1/(2*M_PI*frequency), 2)/divider;

            // if both buttons are pressed consider empty LC metering
            // and recompute the oscillator inductance
            if(!BUTTONA && !BUTTONB){
                inductor = frequency;
            }

            // substract the value of internal inductor/capacitor
            frequency -= (!BUTTONA)?inductor:CAPACITOR;

            // negative result means we got too much noise in the input signal
            dtostre((frequency >= 0.0) ? frequency : 0.0, freq_buffer, 8, 0);

            lcd_write(freq_buffer);
            lcd_line(1);
            lcd_mode(LCD_DATA);
            if(!BUTTONA) lcd_write("L= ");
            else lcd_write("C= ");

            // write the exponent
            lcd_write(freq_buffer+9);
            if(!BUTTONA) lcd_put('H');
            else lcd_put('F');
            continue;
        }

        /* frequency measuring */
        else{
            // prepare frequency text
            dtostre(frequency, freq_buffer, 8, 0);
            lcd_write(freq_buffer);

            // prepare prescaler text
            itoa(_BV(divider_ctrl[divider_id].shift), presc_buffer+1, 10);

            lcd_line(1);
            lcd_mode(LCD_DATA);
            lcd_write(presc_buffer);
            int i;
            for(i=0; i<5-strlen(presc_buffer); i++) lcd_put(' ');
            lcd_write(freq_buffer+10);
        }

        calibrationMode = 0;
    }
}
