/*
 * Project: Tiny Electronic Dice
 * Author: Zak Kemble, contact@zakkemble.co.uk
 * Copyright: (C) 2018 by Zak Kemble
 * License: 
 * Web: http://blog.zakkemble.co.uk/a-tiny-electronic-dice/
 */

#include <stdint.h>
#include <avr/io.h>
#include <avr/power.h>
#include <avr/sleep.h>
#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <util/delay.h>

#define VLOWBATT	2400
#define VREF		1100
#define LOWBATT_VAL	(VREF / (VLOWBATT / 255))

#define WDT_INT_RESET()		(WDTCR |= _BV(WDIE)|_BV(WDE)) // NOTE: Setting WDIE also enables global interrupts
#define WDT_TIMEDOUT() (!(WDTCR & _BV(WDIE)))

#define BTN_ISPRESSED()	(!(PINB & _BV(PINB3)))
#define ROLLDELAY_COUNT	(sizeof(rollDelays) / sizeof(uint8_t))
#define TMR_MS(ms)	((uint8_t)((ms / 16) + 0.5)) // WDT increments every 16ms

typedef enum
{
	BATT_OK = 0,
	BATT_DOCHECK,
	BATT_LOWWARN,
	BATT_LOW
} batt_t;

typedef enum
{
	BTN_NOTPRESSED = 0,
	BTN_PRESSED,
	BTN_DEBOUNCING
} btn_t;

typedef enum
{
	STATE_IDLE = 0,
	STATE_ROLL,
	STATE_ROLLDONE,
	STATE_STEADY
} state_t;

static uint8_t portVals[] = {
	0,
	_BV(PORTB4),
	_BV(PORTB1),
	_BV(PORTB1)|_BV(PORTB4),
	_BV(PORTB1)|_BV(PORTB2),
	_BV(PORTB1)|_BV(PORTB2)|_BV(PORTB4),
	_BV(PORTB0)|_BV(PORTB1)|_BV(PORTB2)
};

static uint8_t rollDelays[] = {
	0,
	TMR_MS(50),
	TMR_MS(100),
	TMR_MS(200),
	TMR_MS(400)
};

// https://www.avrfreaks.net/forum/random-number-generation-0
// https://en.wikipedia.org/wiki/Xorshift
// https://en.wikipedia.org/wiki/Linear_congruential_generator
//uint8_t val = rand() / (RAND_MAX / 6 + 1);
static uint8_t genRandom(void)
{
	// Source: https://github.com/edrosten/8bit_rng/blob/master/rng-4261412736.c
	// Copyright Edward Rosten 2008--2013.

	static uint8_t x;
	static uint8_t y;
	static uint8_t z;
	static uint8_t a = 86; // Seed value

	uint8_t t = x ^ (x << 4);
	x = y;
	y = z;
	z = a;
	a = z ^ t ^ (z >> 1) ^ (t << 1);

	return a;
}

static uint8_t battLevel(void)
{
	power_adc_enable();
	ADCSRA |= _BV(ADEN);
	_delay_us(70); // Allow time for Vbg to startup
	ADCSRA |= _BV(ADSC);
	while(ADCSRA & _BV(ADSC));
	uint8_t val = ADCH;
	ADCSRA &= ~_BV(ADEN);
	power_adc_disable();
	return val;
}

static void diceSet(uint8_t val)
{
	PORTB = (PORTB & ~(_BV(PORTB0)|_BV(PORTB1)|_BV(PORTB2)|_BV(PORTB4))) | portVals[val];
}

void get_mcusr(void) __attribute__((naked)) __attribute__((section(".init3")));
void get_mcusr()
{
	MCUSR = 0;
	wdt_disable();
}

int main(void)
{
	clock_prescale_set(CPU_DIV);

	ADMUX = _BV(ADLAR)|_BV(MUX3)|_BV(MUX2); // Left adjust result for 8bit, measure internal Vbg (1.1V)
	ADCSRA = _BV(ADPS2)|_BV(ADPS1)|_BV(ADPS0); // 128 divisor

	ACSR = _BV(ACD); // Power off analogue comparator
	power_all_disable(); // Power off everything else

	set_sleep_mode(SLEEP_MODE_PWR_DOWN);

	PORTB |= _BV(PORTB3); // Enable pullup
	DDRB |= _BV(DDB0)|_BV(DDB1)|_BV(DDB2)|_BV(DDB4); // Set as outputs

	// Run genRandom() a few times to get it going properly
	for(uint8_t i=0;i<battLevel();i++)
		genRandom();

	// Button interrupt
	PCMSK |= _BV(PCINT3);
	GIMSK |= _BV(PCIE);

	uint8_t now = 0;
	uint8_t timer = 0;
	state_t state = STATE_IDLE;
	batt_t lowBatt = BATT_DOCHECK;
	uint8_t rollResult = 0;
	uint8_t nextRollChangeDelay = 0;
	btn_t btnPressState = BTN_NOTPRESSED;
	uint8_t btnReleasedTime = 0;
	uint8_t blinkCount = 0;
	uint8_t blinkState = 0;

	// Enable WDT
	wdt_enable(WDTO_15MS);
	WDT_INT_RESET();

	sei();

	while(1)
	{
		// Timer stuff, increments every 16ms from the WDT
		// Also turns the WDT back on
		if(WDT_TIMEDOUT())
		{
			WDT_INT_RESET();
			now++;
		}

		if(BTN_ISPRESSED())
		{
			if(btnPressState == BTN_NOTPRESSED)
			{
				timer = now;
				blinkCount = 0;
				blinkState = 0;
			}
			btnPressState = BTN_PRESSED;
			nextRollChangeDelay = 1;
			state = STATE_ROLL;
		}
		else
		{
			// Debouncing on button release
			if(btnPressState == BTN_PRESSED)
			{
				btnReleasedTime = now;
				btnPressState = BTN_DEBOUNCING;
			}
			else if((uint8_t)(now - btnReleasedTime) >= TMR_MS(50))
				btnPressState = BTN_NOTPRESSED;
		}

		if(lowBatt == BATT_DOCHECK)
			lowBatt = (battLevel() > LOWBATT_VAL) ? BATT_LOWWARN : BATT_OK; // Some backwards logic here since we're measuring the internal VREF against VCC

		if(lowBatt == BATT_LOWWARN)
		{
			if((uint8_t)(now - timer) >= TMR_MS(32))
			{
				timer = now;
				blinkState = !blinkState;
				if(!blinkState)
					diceSet(1);
				else
				{
					diceSet(0);

					blinkCount++;
					if(blinkCount > 5)
					{
						blinkCount = 0;
						blinkState = 0;
						lowBatt = BATT_LOW;
					}
				}
			}
		}
		else if(state == STATE_ROLL)
		{
			if((uint8_t)(now - timer) >= rollDelays[nextRollChangeDelay])
			{
				timer = now;
				nextRollChangeDelay++;

				if(nextRollChangeDelay >= ROLLDELAY_COUNT)
				{
					state = STATE_ROLLDONE;
					rollResult = (genRandom() % 6) + 1;
					diceSet(rollResult);
				}
				else
				{
					rollResult++;
					if(rollResult > 6)
						rollResult = 1;
					diceSet(rollResult);
				}
			}
		}
		else if(state == STATE_ROLLDONE)
		{
			if(!blinkState || (uint8_t)(now - timer) >= blinkState)
			{
				timer = now;
				if(blinkState == TMR_MS(200))
				{
					blinkState = TMR_MS(50);
					diceSet(0);
				}
				else
				{
					blinkState = TMR_MS(200);
					diceSet(rollResult);

					blinkCount++;
					if(blinkCount > 3)
					{
						blinkCount = 0;
						blinkState = 0;
						state = STATE_STEADY;
					}
				}
			}
		}
		else if(state == STATE_STEADY)
		{
			if((uint8_t)(now - timer) >= TMR_MS(2000))
			{
				diceSet(0);
				state = STATE_IDLE;
			}
		}

		// Sleep if nothing to do
		cli();
		if(!WDT_TIMEDOUT())
		{
			if(!BTN_ISPRESSED() && btnPressState == BTN_NOTPRESSED && state == STATE_IDLE && lowBatt != BATT_LOWWARN)
			{
				wdt_disable();
				lowBatt = BATT_DOCHECK;
			}

			sleep_enable();
			//sleep_bod_disable();
			sei();
			sleep_cpu();
			sleep_disable();
		}
		sei();
	}

}

EMPTY_INTERRUPT(WDT_vect);
EMPTY_INTERRUPT(PCINT0_vect);
