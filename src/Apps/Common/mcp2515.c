#include <avr/pgmspace.h>
#include <util/delay_basic.h>
#include "utils.h"
#include "mcp2515.h"
#include "spi.h"


#ifndef	MCP2515_CS
	#error	MCP2515_CS ist nicht definiert!
#endif


#ifndef	MCP2515_BITRATE
	#error	MCP2515_BITRATE not defined!
#else
	#if	MCP2515_BITRATE == 125
		// 125 kbps
		#define	R_CNF3	((1<<PHSEG21))
		#define	R_CNF2	((1<<BTLMODE)|(1<<PHSEG11))
		#define	R_CNF1	((1<<BRP2)|(1<<BRP1)|(1<<BRP0))
	#elif MCP2515_BITRATE == 250
		// 250 kbps
		#define	R_CNF3	((1<<PHSEG21))
		#define	R_CNF2	((1<<BTLMODE)|(1<<PHSEG11))
		#define	R_CNF1	((1<<BRP1)|(1<<BRP0))
	#elif MCP2515_BITRATE == 500
		// 500 kbps
		#define	R_CNF3	((1<<PHSEG21))
		#define	R_CNF2	((1<<BTLMODE)|(1<<PHSEG11))
		#define	R_CNF1	((1<<BRP0))
	#elif MCP2515_BITRATE == 1000
		// 1 Mbps
		#define	R_CNF3	((1<<PHSEG21))
		#define	R_CNF2	((1<<BTLMODE)|(1<<PHSEG11))
		#define	R_CNF1	(0)
	#else
		#error invalid value for MCP2515_BITRATE
	#endif
#endif


// Registersatz aufsetzen

static const uint8_t PROGMEM mcp2515_register_map[] = {
	R_CNF3,
	R_CNF2,
	R_CNF1,
	MCP2515_INTERRUPTS,
	0							// clear interrupt flags
};


int8_t mcp2515_read_register(uint8_t adress)
{
	uint8_t data;
	
	RESET(MCP2515_CS);
	
	spi_putc(SPI_READ);
	spi_putc(adress);
	
	data = spi_putc(0xff);	
	
	SET(MCP2515_CS);
	
	return data;
}

void mcp2515_write_register(uint8_t adress, uint8_t data )
{
	RESET(MCP2515_CS);
	spi_putc(SPI_WRITE);
	spi_putc(adress);
	spi_putc(data);
	SET(MCP2515_CS);
}

uint8_t mcp2515_read_status(uint8_t type)
{
	uint8_t data;
	
	RESET(MCP2515_CS);
	
	spi_putc(type);
	data = spi_putc(0xff);
	
	SET(MCP2515_CS);
	
	return data;
}

uint8_t mcp2515_read_id(uint32_t *id)
{
	uint8_t first;
	uint8_t tmp;
	
	first = spi_putc(0xff);
	tmp   = spi_putc(0xff);
	
	if (tmp & (1 << IDE)) {
	  // Nur Extended IDs empfangen
		*((uint16_t *) id + 1)  = (uint16_t) first << 5;
		*((uint8_t *)  id + 1)  = spi_putc(0xff);
		*((uint8_t *)  id + 2) |= (tmp >> 3) & 0x1C;
		*((uint8_t *)  id + 2) |=  tmp & 0x03;
		*((uint8_t *)  id)      = spi_putc(0xff);
		return TRUE;
	} else {
		return FALSE;
	}
}

void mcp2515_write_id(const uint32_t *id)
{
	uint8_t tmp;
	
	spi_putc(*((uint16_t *) id + 1) >> 5);
	
	// naechsten Werte berechnen
	tmp  = (*((uint8_t *) id + 2) << 3) & 0xe0;
	tmp |= (1 << IDE);
	tmp |= (*((uint8_t *) id + 2)) & 0x03;
	
	// restliche Werte schreiben
	spi_putc(tmp);
	spi_putc(*((uint8_t *) id + 1));
	spi_putc(*((uint8_t *) id));
}


uint8_t mcp2515_send_message(const can_t *msg)
{
  // Status des MCP2515 auslesen
  uint8_t status;
	
  /* Statusbyte:
   *
   * Bit	Funktion
   *  2	TXB0CNTRL.TXREQ
   *  4	TXB1CNTRL.TXREQ
   *  6	TXB2CNTRL.TXREQ
   */
  uint8_t address;
  address = 0xff ;
  
  while (address==0xff) {
    // solange versuchen, bis ein Puffer freigeworden ist 
    status = mcp2515_read_status(SPI_READ_STATUS);
    
    if (_bit_is_clear(status, 2)) {
      address = 0x00;
    }
    else if (_bit_is_clear(status, 4)) {
      address = 0x02;
    } 
    else if (_bit_is_clear(status, 6)) {
      address = 0x04;
    } ;
  }
  
  RESET(MCP2515_CS);
  
  spi_putc(SPI_WRITE_TX | address);
  
  mcp2515_write_id(&msg->id);
  
  uint8_t length = msg->length & 0x0f;
  
  // Nachrichten Laenge einstellen
  spi_putc(length);
  
  // Daten
  for (uint8_t i=0;i<length;i++) {
    spi_putc(msg->data[i]);
  }
  SET(MCP2515_CS);
  
  asm volatile ("nop");
  asm volatile ("nop");
  
  // CAN Nachricht verschicken
  // die letzten drei Bit im RTS Kommando geben an welcher
  // Puffer gesendet werden soll.
  RESET(MCP2515_CS);
  address = (address == 0) ? 1 : address;
  spi_putc(SPI_RTS | address);
  SET(MCP2515_CS);
  
  return address;
}

void mcp2515_bit_modify(uint8_t adress, uint8_t mask, uint8_t data)
{
	RESET(MCP2515_CS);
	
	spi_putc(SPI_BIT_MODIFY);
	spi_putc(adress);
	spi_putc(mask);
	spi_putc(data);
	
	SET(MCP2515_CS);
}


uint8_t mcp2515_get_message(can_t *msg)
{
  uint8_t addr,i;
	
	if (IS_SET(MCP2515_INT)) return(NO_MESSAGE); // Kein Interrupt gesetzt, damit auch keine Nachricht vorhanden...

	// read status
	uint8_t status = mcp2515_read_status(SPI_RX_STATUS);
	
	if (_bit_is_set(status,6)) {
		// message in buffer 0
		addr = SPI_READ_RX;
	}
	else if (_bit_is_set(status,7)) {
	// message in buffer 1
		addr = SPI_READ_RX | 0x04;
	}
	else {
		// Error: no message available
		return(NO_MESSAGE);
	}
	
	RESET(MCP2515_CS);
	spi_putc(addr);
	
	// CAN ID auslesen und ueberpruefen
	mcp2515_read_id(&msg->id);
	
	// Laenge auslesen
	uint8_t length = spi_putc(0xff);
	
	length &= 0x0f;
	msg->length = length;
	// Daten auslesen
	for (i=0;i<length;i++) {
		msg->data[i] = spi_putc(0xff);
	}
	for (;i<8;i++) msg->data[i]=0 ; // Rest der Nachricht nullen

	SET(MCP2515_CS);
	
	// Interrupt zuruecksetzen
	if (_bit_is_set(status, 6))
		mcp2515_bit_modify(CANINTF, (1<<RX0IF), 0);
	else
		mcp2515_bit_modify(CANINTF, (1<<RX1IF), 0);
	
	return (SUCCESSFULL_RESPONSE);
}


void mcp2515_init(void)
{
	mcp2515_spi_init() ;
	SET_INPUT_WITH_PULLUP(MCP2515_INT);
	
	// Aktivieren des SPI Master Interfaces
	
	// MCP2515 per Software Reset zuruecksetzten,
	// danach ist er automatisch im Konfigurations Modus
	RESET(MCP2515_CS);
	spi_putc(SPI_RESET);
	SET(MCP2515_CS);
	
	// ein bisschen warten bis der MCP2515 sich neu gestartet hat
	_delay_loop_2(1000);
	
	// Filter usw. setzen
	RESET(MCP2515_CS);
	spi_putc(SPI_WRITE);
	spi_putc(CNF3);
	for (uint8_t i = 0; i < sizeof(mcp2515_register_map); i++)
		spi_putc(pgm_read_byte(&mcp2515_register_map[i]));
	SET(MCP2515_CS);
	
	// nur Extended IDs mit Filter, Message Rollover nach Puffer 1
	mcp2515_write_register(RXB0CTRL, (1<<RXM1)|(0<<RXM0)|(1<<BUKT));
	mcp2515_write_register(RXB1CTRL, (1<<RXM1)|(0<<RXM0));
	
	// MCP2515 zurueck in den normalen Modus versetzten
	mcp2515_write_register(CANCTRL, 0);
}
