#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <util/delay.h>
#include <avr/boot.h>
#include <avr/pgmspace.h>
#include <avr/eeprom.h>
#include "common_define.h"


//configuring LAST_INTVECT_ADDRESS as per device selected
/*****************************************************************************/
#define LAST_INTVECT_ADDRESS 		TWI_vect // The start of the application code


/*****************************************************************************/
#define TWI_SLAW_RECEIVED         	0x60	// Status slave address and write command received
#define TWI_SLAR_RECEIVED         	0xa8	// Status slave address and read command received
#define TWI_SLAVE_TX_NACK_RECEIVED 	0xc0	// Status slave transmit and no acknowledgement or last byte
#define TWI_SLAVE_RX_ACK_RETURNED  	0x80	// Status slave receive and acknowledgement returned
#define TWI_SLAVE_RX_NACK_RETURNED 	0x88	// Status slave receive and no acknowledgement or last byte

// 10 s delay code for allowing ARM9 linux to boot
#define HOST_BOOT_DELAY_SEC 5

#define ACK TWI_SLAVE_RX_ACK_RETURNED
#define NAK TWI_SLAVE_RX_NACK_RETURNED


void init_twi (void) {
    DDRC &= ~(_BV(PORTC5) | _BV(PORTC4)); // Set SCL and SDA as input
    PORTC &= ~(_BV(PORTC5) | _BV(PORTC4)); // Set SCL and SDA low
    // Note: PORTC4 and PORT5 commonly used for tiny48. tiny88, mega48 TWI based devices

    TWAR = SLAVE_ADDRESS;
    TWCR = _BV(TWEN);
    // Enable, but don't enable ACK until we are ready to receive packets.
}




void wait_for_activity(void) {
    do {} while ((TWCR & _BV(TWINT)) == 0);
    wdt_reset ();
}

uint8_t get_status_code (void) {
    // Check if SPM operation is complete
    if ((SPMCSR & _BV(SELFPROGEN)) != 0)
        return STATUSMASK_SPMBUSY;

    return 0;
}

void abort_twi (void) {
    // Recover from error condition by releasing bus lines.
    TWCR = _BV(TWINT) | _BV(TWSTO) | _BV(TWEN);
}

void process_slave_transmit (uint8_t data) {
    // Prepare data for transmission.
    TWDR = data;
    TWCR = _BV(TWINT) | _BV(TWEN);	// Send byte, NACK expected from master.
    wait_for_activity();

    // Check TWI status code for SLAVE_TX_NACK.
    if ( TWSR != TWI_SLAVE_TX_NACK_RECEIVED ) {
        abort_twi ();
        return;
    }

    // End communication.
    TWCR = _BV(TWINT) | _BV(TWEN);
}

uint8_t slave_receive_byte (uint8_t * data, uint8_t ack) {
    // Receive byte and return ACK.
    TWCR = _BV(TWINT) | _BV(TWEN) | ( ack == ACK  ? _BV(TWEA) : 0 );

    wait_for_activity();

    // Check TWI status code for SLAVE_RX_ACK. or SLAVE_RX_NACK
    // Basically, if the status register has the same value as
    // the type of packet we're looking for, then proceeed

    if ( TWSR == ack) {
        // Get byte
        *data = TWDR;
        if (ack == TWI_SLAVE_RX_NACK_RETURNED) {
            // If we're doing a NACK, then twiddle TWCR
            TWCR = _BV(TWINT) | _BV(TWEN);
        }
        return 1;
    }
    abort_twi ();
    return 0;
}

void update_page (uint16_t pageAddress) {
    // Mask out in-page address bits.
    pageAddress &= ~(PAGE_SIZE - 1);
    // Protect RESET vector if this is page 0.
    if (pageAddress == INTVECT_PAGE_ADDRESS) {
        // Load existing RESET vector contents into buffer.
        pageBuffer[0] = pgm_read_word(INTVECT_PAGE_ADDRESS + 0);
        pageBuffer[1] = pgm_read_word(INTVECT_PAGE_ADDRESS + 1);
    }

    // Ignore any attempt to update boot section.
    if (pageAddress >= BOOT_PAGE_ADDRESS) {
        return;
    }
    boot_page_erase (pageAddress);
    boot_spm_busy_wait();

    for (uint8_t i = 0; i < PAGE_SIZE; i += 2) {
        uint16_t tempWord = ((pageBuffer[i+1] << 8) | pageBuffer[i]);
        boot_page_fill (pageAddress + i, tempWord); // Fill the temporary buffer with the given dataa
        boot_spm_busy_wait();
    }
    // Write page from temporary buffer to the given location in flash memory
    boot_page_write (pageAddress);
    boot_spm_busy_wait();

    wdt_reset (); // Reset the watchdog timer
}

void process_page_update (void) {
    // Check the SPM is ready, abort if not.
    if ((SPMCSR & _BV(SELFPROGEN)) != 0) {
        abort_twi ();
        return;
    }
    uint8_t pageAddressLo;
    uint8_t pageAddressHi;
    uint8_t *bufferPtr = pageBuffer;

    // Receive two-byte page address.
    if (slave_receive_byte (&pageAddressLo, ACK) ) {
        if (slave_receive_byte (&pageAddressHi, ACK) ) {
            // Receive page data.
            for (uint8_t i = 0; i < (PAGE_SIZE - 1); ++i) {
                if (slave_receive_byte (bufferPtr, ACK)) {
                    ++bufferPtr;
                } else {
                    return;
                }
            }

            if (slave_receive_byte(bufferPtr, NAK) ) {
                // Now program if everything went well.
                update_page ((pageAddressHi << 8) | pageAddressLo);
            }
        }
    }
}

void cleanup_and_run_application (void) {
    wdt_disable(); // After Reset the WDT state does not change
    // Set up function pointer to address after last interrupt vector.
//  void (*FuncPtr) (void) = (void (*)(void)) ((LAST_INTVECT_ADDRESS + 2) / 2);
// FuncPtr ();

    asm volatile ("rjmp __vectors - 4"); // jump to application reset vector at end of flash

    for (;;); // Make sure function does not return to help compiler optimize

}

void process_page_erase (void) {
    uint16_t addr = 0;
    for (uint8_t i = 0; i < PAGE_SIZE; ++i) {
        pageBuffer[i] = 0xFF;
    }

    update_page (addr);		// To restore reset vector
    addr += PAGE_SIZE;

    for (uint8_t i = 0; i < (LAST_PAGE_NO_TO_BE_ERASED - 1); i++, addr += PAGE_SIZE) {
        addr &= ~(PAGE_SIZE - 1);

        if (addr < BOOT_PAGE_ADDRESS)
            // Erase each page one by one until the bootloader section
            boot_page_erase (addr);
            boot_spm_busy_wait();
        }
    }
}

void process_slave_receive (void) {
    uint8_t commandCode;

    if (! slave_receive_byte (&commandCode, ACK) ) {
        return;
    }
    // Process command byte.
    switch (commandCode) {
    case TWI_CMD_PAGEUPDATE:
        process_page_update ();
        break;
    case TWI_CMD_EXECUTEAPP:
        // Read dummy byte and NACK, just to be nice to our TWI master.
        slave_receive_byte (&commandCode, NAK);
        wdt_enable(WDTO_15MS  );  // Set WDT min for cleanup using reset
        for (;;); // Wait for WDT reset

    case TWI_CMD_ERASEFLASH:
        slave_receive_byte (&commandCode, NAK);
        process_page_erase ();
        break;

    default:
        abort_twi ();
    }
}

void read_and_process_packet (void) {
    // Enable ACK and clear pending interrupts.
    TWCR = _BV(TWINT) | _BV(TWEA) | _BV(TWEN);

    wait_for_activity();


    // Check TWI status code for SLA+W or SLA+R.
    switch (TWSR) {
    case TWI_SLAW_RECEIVED:
        process_slave_receive ();
        break;

    case TWI_SLAR_RECEIVED:
        process_slave_transmit (get_status_code () & STATUSMASK_SPMBUSY);
        break;

    default:
        abort_twi ();
    }
}

void start_timer (void) {
    TIFR1   = TIFR1;  // Clear flags
    TCNT1H  = 0;
    TCNT1L  = 0;
    OCR1A   = 7813;   // 7812.5 (one sec at 8 MHz clock operation)
    TCCR1B  = _BV (CS12) + _BV (CS10) + _BV (WGM12);	// mode4: CTC,
    // Prescaller:1024
}

void host_boot_delay () {
    uint8_t time_lapse_sec = HOST_BOOT_DELAY_SEC;

    start_timer ();
    do {
        if (TIFR1 & _BV (OCF1A)) {
            time_lapse_sec--;
            TIFR1 = TIFR1;
        }
    } while (time_lapse_sec > 0);
    TCCR1B = 0;
    // stop the timer NOW

}

// Main Starts from here
int main (void) {
    if (MCUSR & _BV (PORF)) {	// Only in case of Power On Reset
        MCUSR = 0;
        host_boot_delay ();
        init_twi();
        wdt_enable(WDTO_8S);

        while (1) {
            read_and_process_packet (); // Process the TWI Commands
        }
    } else {
        cleanup_and_run_application ();
    }


}
