// ----------------------------------------------------------------------------
/*
 * Copyright (c) 2007 Fabian Greif, Roboterclub Aachen e.V.
 *  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $Id: mcp2515_send_message.c 6837 2008-11-16 19:05:15Z fabian $
 */
// ----------------------------------------------------------------------------

#include "mcp2515_private.h"

#include <util/delay.h>

// ----------------------------------------------------------------------------
uint8_t mcp2515_send_message(uint8_t IF,const can_t *msg)
{
	// Status des MCP2515 auslesen
	uint8_t status = mcp2515_read_status(IF,SPI_READ_STATUS);
	
	/* Statusbyte:
	 *
	 * Bit	Funktion
	 *  2	TXB0CNTRL.TXREQ
	 *  4	TXB1CNTRL.TXREQ
	 *  6	TXB2CNTRL.TXREQ
	 */
	uint8_t address;
	if (_bit_is_clear(status, 2)) {
		address = 0x00;
	}
	else if (_bit_is_clear(status, 4)) {
		address = 0x02;
	} 
	else if (_bit_is_clear(status, 6)) {
		address = 0x04;
	}
	else {
		// Alle Puffer sind belegt,
		// Nachricht kann nicht verschickt werden
		return 0;
	}
	
	if (IF==0) {
		RESET(MCP2515_CS_1);
	} else {
		RESET(MCP2515_CS_2);
	} ;
	spi_putc(IF,SPI_WRITE_TX | address);
	mcp2515_write_id(IF,&msg->id, msg->flags.extended);
	uint8_t length = msg->length & 0x0f;
	
	// Ist die Nachricht ein "Remote Transmit Request" ?
	if (msg->flags.rtr)
	{
		// Ein RTR hat zwar eine Laenge,
		// enthaelt aber keine Daten
		
		// Nachrichten Laenge + RTR einstellen
		spi_putc(IF,(1<<RTR) | length);
	}
	else
	{
		// Nachrichten Laenge einstellen
		spi_putc(IF,length);
		
		// Daten
		for (uint8_t i=0;i<length;i++) {
			spi_putc(IF,msg->data[i]);
		}
	}
	if (IF==0) {
		SET(MCP2515_CS_1);
	} else {
		SET(MCP2515_CS_2);
	} ;
	
	_delay_us(1);
	
	// CAN Nachricht verschicken
	// die letzten drei Bit im RTS Kommando geben an welcher
	// Puffer gesendet werden soll.
	if (IF==0) {
		RESET(MCP2515_CS_1);
	} else {
		RESET(MCP2515_CS_2);
	} ;
	address = (address == 0) ? 1 : address;
	spi_putc(IF,SPI_RTS | address);
	if (IF==0) {
		SET(MCP2515_CS_1);
	} else {
		SET(MCP2515_CS_2);
	} ;
	
	CAN_INDICATE_TX_TRAFFIC_FUNCTION;
	
	return address;
}

