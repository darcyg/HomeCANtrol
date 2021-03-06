/*

  LED.c Steuert das LED-Board an; MC: At90PWM3B mit 16 MHz

*/
 
// Defines an den Controller und die Anwendung anpassen
 
// includes
 
#include <stdint.h>
#include <string.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/eeprom.h>
#include <avr/wdt.h>
#include <util/delay.h>
#include "../Common/mcp2515.h"


/* EEProm-Belegung vom Boot-Loader:
0   0xba
1   0xca
2   BoardAdd Low Byte
3   BoardAdd High Byte
4   BoardLine
5   BootAdd Low Byte
6   BootAdd High Byte
7   BootLine
8   BoardType (0: LED, 0x10: Relais, 0x20: Sensor)  
9   Group 1

EEProm-Belegung vom LED-Board:
10..29: Programm Channel 1
30..49: Programm Channel 2,...
450..469: Programm Channel 22
*/
// globale Variablen

#define PWM_CHANNELS  24                 // Anzahl der PWM-Kanaele
#define TIMER_PRESET 99

extern void InitBCM(void);

extern volatile uint8_t LEDVal[PWM_CHANNELS] ;
extern volatile uint8_t LEDTargetVal[PWM_CHANNELS] ;
extern volatile uint8_t LEDPWM[56] ;
extern volatile uint8_t MasterVal ;
volatile uint16_t LEDV2[PWM_CHANNELS] ;
volatile int16_t Delta[PWM_CHANNELS] ;
volatile uint8_t Counter[PWM_CHANNELS] ;
volatile uint8_t Step[PWM_CHANNELS] ;

can_t Message ;
uint8_t  BoardLine ;
uint16_t BoardAdd ;

uint8_t GlobalTime ;
uint8_t LastTime[PWM_CHANNELS] ;

volatile uint8_t  Heartbeat ;
volatile uint16_t Timers ;
uint8_t GlobalDimTimer = 1 ;

const uint16_t LED_TO_R [] PROGMEM = { 0,9,1,18,12,4,7} ;
const uint16_t LED_TO_G [] PROGMEM = { 8,10,2,19,13,5,15} ;
const uint16_t LED_TO_B [] PROGMEM = { 16,11,3,20,14,6,21} ;

// BuildCANId baut aus verschiedenen Elementen (Line & Addresse von Quelle und Ziel 
// sowie Repeat-Flag und Gruppen-Flag) den CAN Identifier auf

inline uint32_t BuildCANId (uint8_t Prio, uint8_t Repeat, uint8_t FromLine, uint16_t FromAdd, uint8_t ToLine, uint16_t ToAdd, uint8_t Group)
{
  return (((uint32_t)(Group&0x1))<<1|((uint32_t)ToAdd)<<2|((uint32_t)(ToLine&0xf))<<10|
	  ((uint32_t)FromAdd)<<14|((uint32_t)(FromLine&0xf))<<22|
	  ((uint32_t)(Repeat&0x1))<<26|((uint32_t)(Prio&0x3))<<27) ;
}

// GetSourceAddress bestimmt aus dem CAN-Identifier der eingehenden Nachricht 
// die Line & Addresse

inline void GetSourceAddress (uint32_t CANId, uint8_t *FromLine, uint16_t *FromAdd)
{
  *FromLine = (uint8_t)((CANId>>22)&0xf) ;
  *FromAdd = (uint16_t) ((CANId>>14)&0xfff) ;
}

// Alle Filter des 2515 auf die eigene Board-Addresse setzen

void SetFilter(uint8_t BoardLine,uint16_t BoardAdd)
{
  can_filter_t filter ;
  filter.id = ((uint32_t)BoardAdd)<<2|((uint32_t)BoardLine)<<10 ;
  filter.mask = 0x3FFC ;
  mcp2515_set_filter(0, &filter) ;
  mcp2515_set_filter(1, &filter) ;
  mcp2515_set_filter(2, &filter) ;
  mcp2515_set_filter(3, &filter) ;
  mcp2515_set_filter(4, &filter) ;
  filter.id = ((uint32_t)0xff)<<2|((uint32_t)BoardLine)<<10 ;
  mcp2515_set_filter(5, &filter) ;
}


// Message fuer das zuruecksenden vorbereiten (Quelle als Ziel eintragen und 
// Boardaddresse als Quelle)

void SetOutMessage (uint8_t BoardLine, uint16_t BoardAdd)
{
  uint8_t SendLine ;
  uint16_t SendAdd ;
  
  GetSourceAddress(Message.id,&SendLine,&SendAdd) ;
  Message.id = BuildCANId (0,0,BoardLine,BoardAdd,SendLine,SendAdd,0) ;
  Message.data[0] = Message.data[0]|SUCCESSFULL_RESPONSE ;
  Message.length = 1 ;
}


uint8_t GetProgram(uint8_t Channel, uint8_t PStep)
{
  uint8_t *Da ;

  if (Channel>22) return(0) ;  
  if (PStep>=20) return (0) ;
  Da = (uint8_t*) 10 ;
  Da += Channel*20;
  Da += PStep ;
  return (eeprom_read_byte(Da)) ;
}

uint16_t CalcDelta (uint8_t Chan, int32_t New,int16_t Time)
{
  int16_t Delta ;
  Delta = (int16_t)(((((int32_t)New)<<8)-(int32_t)LEDV2[Chan])/(int16_t)Time) ;
  if (Delta>0) Delta++ ; // Rundungsfehler ausgleichen	
  LEDV2[Chan]+=Delta ;
  return (Delta) ;
}


inline void StepLight (void)
{
  uint8_t Channel ;
  uint8_t Command ;
  
  // Alle Kanaele abarbeiten; wird 25 mal pro sekunde aufgerufen 
  for (Channel=0;Channel<PWM_CHANNELS;Channel++) {
    if (Counter[Channel]>0) {
      if (Delta[Channel]!=0x7FFF) {
	LEDV2[Channel] += Delta[Channel] ;
	Counter[Channel]-- ;
	if (Counter[Channel]==0) LEDV2[Channel]=((unit16_t)LEDTargetVal[Channel]<<8) ;
      } else {
	if (Counter[Channel]==GlobalTime) Counter[Channel]=0 ;
      } ;
    } else {
      if (Step[Channel]>=20) continue ;
      Command = GetProgram (Channel,Step[Channel]) ;
      Step[Channel]++ ;
      if (Command==0) { // Programm Ende  
	Step[Channel] = 20 ;
      } else if	(Command<201) { // DimTo 
	Counter[Channel] = Command ;  
	Command = GetProgram(Channel,Step[Channel]) ;
	Step[Channel]++ ;
	LEDTargetVal[Channel]=Command ;
	Delta[Channel] = CalcDelta(Channel,Command,Counter[Channel]) ;
	Counter[Channel]-- ;
      } else if (Command<221) { // JumpTo 
	Step[Channel] = Command-201 ;
      } else if (Command==221) { // SetTo
	LEDTargetVal[Channel]= GetProgram(Channel,Step[Channel]))<<8 ;
	LEDV2[Channel] = ((uint16_t)LEDTargetVal[Channel])<<8 ;
	Step[Channel]++ ;
      } else if (Command==222) { // Delay 
	Delta[Channel] = 0 ;
	Counter[Channel] = GetProgram(Channel,Step[Channel]) ;
	Step[Channel]++ ;
      } else if (Command==223) { // Random Delay 
	Delta[Channel] = 0 ;
	Counter[Channel] = (uint8_t)(TCNT1&0xff) ;
	Step[Channel]++ ;
      } else if (Command==224) {
	// nop
      } else if (Command==225) {
	// Wait for timer 
	Counter[Channel] = LastTime[Channel] + GetProgram(Channel,Step[Channel]) ;
	LastTime[Channel] = Counter[Channel] ;
	Delta[Channel] = 0x7FFF ;
	Step[Channel]++ ;
      } else { // Unknown command 
	Step[Channel] = 20 ; // Auf Ende Setzen 
      } ;
    } ;
    // Uebertragen auf die LED-Ausgabe
    LEDVal[Channel] = (uint8_t)(LEDV2[Channel]>>8) ;
  } ;
}

ISR( TIMER0_OVF_vect )                           
{
  static int CC=0 ;
  
  TCNT0 = (uint8_t)TIMER_PRESET;  // preload for 10ms
  
  if (Timers>0) Timers-- ;
  CC++ ;
  if (CC>=4) {
    StepLight();
    CC = 0 ;
  }
  
  if (Heartbeat<250) Heartbeat++ ;
}

void SetLED (uint8_t Num, uint8_t R, uint8_t G, uint8_t B, uint8_t W, uint8_t Inv)
{
  uint8_t i1;
  uint8_t RChan,GChan,BChan ;
  
  if (Num>7) return ;
  
  if (Num==7) { /* Alle LEDs setzen, rekursiv */
    for (i1=0;i1<7;i1++) SetLED(i1,R,G,B,W,Inv) ;
    return ;
  } ;

  RChan = pgm_read_word(&(LED_TO_R[Num])) ;
  GChan = pgm_read_word(&(LED_TO_G[Num])) ;
  BChan = pgm_read_word(&(LED_TO_B[Num])) ;

  if (Inv==1) {
    R = R-LEDVal[RChan] ;
    G = G-LEDVal[GChan] ;
    B = B-LEDVal[BChan] ;
  } ;
  Counter[RChan] = 0 ;
  Counter[GChan] = 0 ;
  Counter[BChan] = 0 ;
  LEDVal[RChan] = R ;
  LEDVal[GChan] = G ;
  LEDVal[BChan] = B ;
  LEDTargetVal[RChan] = R ;
  LEDTargetVal[GChan] = G ;
  LEDTargetVal[BChan] = B ;
  LEDV2[RChan] = ((uint16_t) R)<<8 ;
  LEDV2[GChan] = ((uint16_t) G)<<8 ;
  LEDV2[BChan] = ((uint16_t) B)<<8;
  if (Num==0) {
    LEDVal[17] = W ;
    LEDTargetVal[17] = W ;
    LEDV2[17] = ((uint16_t) W)<<8 ;
  } ;
}


void DimLED (uint8_t Num, uint8_t R, uint8_t G, uint8_t B, uint8_t W,uint8_t Timer)
{
  uint8_t i1;
  uint8_t RChan,GChan,BChan ;
  
  if (Num>7) return ;

  if (Timer==0) {
    SetLED (Num,R,G,B,W,0);
    return ;
  } ;
  
  if (Num==7) { /* Alle LEDs setzen, rekursiv */
    for (i1=0;i1<7;i1++) DimLED(i1,R,G,B,W,Timer) ;
    return ;
  } ;

  RChan = pgm_read_word(&(LED_TO_R[Num])) ;
  GChan = pgm_read_word(&(LED_TO_G[Num])) ;
  BChan = pgm_read_word(&(LED_TO_B[Num])) ;

  Counter[RChan] = Timer-1 ;
  Delta[RChan] = CalcDelta(RChan,R,Timer) ;
  LEDTargetVal[RChan] = R ;
  LEDVal[RChan] = (uint8_t)(LEDV2[RChan]>>8) ;


  Counter[GChan] = Timer-1 ;
  Delta[GChan] = CalcDelta(GChan,G,Timer) ;
  LEDTargetVal[GChan] = G ;
  LEDVal[GChan] = (uint8_t)(LEDV2[GChan]>>8) ;

  Counter[BChan] = Timer-1 ;
  Delta[BChan] = CalcDelta(BChan,B,Timer) ;
  LEDTargetVal[BChan] = B ;
  LEDVal[BChan] = (uint8_t)(LEDV2[BChan]>>8) ;

  if (Num==0) {
    Counter[17] = Timer-1 ;
    Delta[17] = CalcDelta(17,W,Timer) ;
    LEDTargetVal[17] = W ;
    LEDVal[17] = (uint8_t)(LEDV2[17]>>8); 
  } ;
}



void hsv_to_rgb (unsigned char h, unsigned char s, unsigned char v,unsigned char *r, unsigned char *g, unsigned char *b)
{
  unsigned char i, f;
  unsigned int p, q, t;
  
  if(s==0) {	
	*r = *g = *b = v;
  } else {	
    i=h/43;
    f=h%43;
    p = (v * (255 - s))/256;
    q = (v * ((10710 - (s * f))/42))/256;
    t = (v * ((10710 - (s * (42 - f)))/42))/256;
      
    switch( i ) {	
	case 0:
	  *r = v; *g = t; *b = p; break;
	case 1:
	  *r = q; *g = v; *b = p; break;
	case 2:
	  *r = p; *g = v; *b = t; break;
	case 3:
	  *r = p; *g = q; *b = v; break;			
	case 4:
	  *r = t; *g = p; *b = v; break;				
	case 5:
	  *r = v; *g = p; *b = q; break;
	}
  }
}

void StoreProgram(void)
{
  uint8_t *Da ;
  uint8_t Offset ;
  Offset = Message.data[2] ;
  if (Offset>15) return ; // Illegal Offset
  
  Da = (uint8_t*)10 ;
  Da += Offset ;
  Da += Message.data[1]*20 ;
  
  eeprom_write_byte(Da++,Message.data[3]) ;
  eeprom_write_byte(Da++,Message.data[4]) ;
  eeprom_write_byte(Da++,Message.data[5]) ;
  eeprom_write_byte(Da++,Message.data[6]) ;
  eeprom_write_byte(Da++,Message.data[7]) ;
} 

int main(void) 
{
  // PWM Port einstellen
  uint8_t r,g,b ;
  uint8_t BoardLine ;
  uint16_t BoardAdd ;
  uint8_t LED ;
  uint16_t Addr ;
  uint8_t  LastCommand ;

  
  BoardAdd = 16 ;
  BoardLine = 1 ;
  
  for (r=0;r<PWM_CHANNELS;r++) {
    Step[r] = 22 ;
    Counter[r] = 0 ;
    LEDVal[r] = 0 ;
    LEDTargetVal[r] = 0 ;
    LEDV2[r] = 0 ;
  } ;

  r = eeprom_read_byte((uint8_t*)0) ;
  if (r==0xba) {
    r = eeprom_read_byte((uint8_t*)1) ;
    if (r==0xca) {
      BoardAdd = eeprom_read_byte((uint8_t*)2) ;
      BoardAdd += ((uint16_t)eeprom_read_byte((uint8_t*)3))<<8 ;
      BoardLine = eeprom_read_byte((uint8_t*)4) ;
    } ;
  } ;
  
  mcp2515_init();
  
  SetFilter(BoardLine,BoardAdd) ;
  
  MasterVal = 255 ;
  
  /* Filter muss hier gesetzt werden */
  
  // Timer 0 als 10 ms-Timer verwenden
  TCCR0B = (1<<CS02)|(1<<CS00);  // divide by 1024
  TCNT0 = (uint8_t)TIMER_PRESET; // preload for 10ms
  TIMSK0 |= 1<<TOIE0;            // enable timer interrupt
  
  InitBCM () ; 
  
  sei();                  // Interrupts gloabl einschalten
  
 
  while(1) {
  
    Message.data[0] = 0 ; // Kommando loeschen

	// Warte auf die n�chste CAN-Message
    while ((LastCommand=mcp2515_get_message(&Message)) == NO_MESSAGE) {

    };
	
    // Sende-Addresse zusammenst�pseln
    r = Message.data[0] ;
    LED = Message.data[1] ;
    SetOutMessage(BoardLine,BoardAdd) ;
    
    switch (r) {
    case SEND_STATUS:
      Message.data[1] = 0 ;
      Message.data[2] = 0 ;
      Message.length = 3 ;
      mcp2515_send_message(&Message) ;				
      break ;
    case READ_CONFIG:
      Message.data[1] = eeprom_read_byte((uint8_t*)0) ;
      Message.data[2] = eeprom_read_byte((uint8_t*)1) ;
      Message.data[3] = eeprom_read_byte((uint8_t*)2) ;
      Message.data[4] = eeprom_read_byte((uint8_t*)3) ;
      Message.data[5] = eeprom_read_byte((uint8_t*)4) ;
      Message.length = 6 ;
      mcp2515_send_message(&Message) ;
      break ;
    case WRITE_CONFIG:
      if ((Message.data[1] == 0xba)&&(Message.data[2]==0xca)) {	
	eeprom_write_byte((uint8_t*)2,Message.data[3]) ;	
	eeprom_write_byte((uint8_t*)3,Message.data[4]) ;	
	eeprom_write_byte((uint8_t*)4,Message.data[5]) ;	
      } ;
      break ;
    case READ_VAR:
      Addr = ((uint16_t)Message.data[1])+(((uint16_t)Message.data[2])<<8) ;
      Message.data[3]=eeprom_read_byte((uint8_t*)Addr) ;
      Message.length = 4 ;
      mcp2515_send_message(&Message) ;
      break ;
    case SET_VAR:
      Addr = ((uint16_t)Message.data[1])+(((uint16_t)Message.data[2])<<8) ;
      eeprom_write_byte((uint8_t*)Addr,Message.data[3]) ;
      Message.length=4 ;
      mcp2515_send_message(&Message) ; // Empfang bestaetigen
      break ;
    case START_BOOT:
      wdt_enable(WDTO_250MS) ;
      while(1) ;
      break ;
    case TIME:
      GlobalTime = Message.data[1] ;
      break ;

    case CHANNEL_OFF:
      //for (g=1;g<7;g++) 
      //SetLED (g,0,0,0,0,0) ;
      SetLED (7,0,0,0,0,0) ;	  
      mcp2515_send_message(&Message) ;
      break ;
    case CHANNEL_ON:
      //for (g=1;g<7;g++) 	
      //SetLED (g,255,255,255,255,0) ;
      SetLED (7,255,255,255,255,0) ;
      mcp2515_send_message(&Message) ;
      break ;
    case CHANNEL_TOGGLE:
      //for (g=1;g<7;g++) 
      //SetLED (g,255,255,255,255,1) ;
      SetLED (7,255,255,255,255,1) ;
      mcp2515_send_message(&Message) ;
      break ;

      // LED Specific
    case SET_TO:
      SetLED (LED,Message.data[2],Message.data[3],Message.data[4],Message.data[5],0) ;
      break ;
    case HSET_TO:
      hsv_to_rgb(Message.data[2],Message.data[3],Message.data[4],&r,&g,&b) ;
      SetLED (LED,r,g,b,Message.data[5],0) ;
      mcp2515_send_message(&Message) ;
      break ;
    case L_AND_S:
      StoreProgram() ;
      break ;
    case LOAD_TWO_LED:
      DimLED (LED,Message.data[2],Message.data[3],Message.data[4],0,GlobalDimTimer) ;
      if (LED<6) DimLED (LED+1,Message.data[5],Message.data[6],Message.data[7],0,GlobalDimTimer) ;
      break ;
    case LOAD_PROG:
      if (Message.data[1]>PWM_CHANNELS) {
	for (r=0;r<PWM_CHANNELS;r++) {
	  Message.data[1] = r ;
	  StoreProgram () ;
	} ;
      } else {
	Message.data[1]-- ;
	StoreProgram() ;
      } ;
      break ;
    case START_PROG:
      if (Message.data[1]>PWM_CHANNELS) {
	for (r=0;r<PWM_CHANNELS;r++) {
	  Step[r] = Message.data[2] ;
	  Counter[r] = Message.data[7] ;
	  LastTime[r] = Counter[r] ;
	  Delta[r]=0x7FFF ;
	} ;
      } else {
	r = Message.data[1]-1 ;
        Step[r] = Message.data[2] ;
	Counter[r] = Message.data[7] ;
	LastTime[r] = Counter[r] ;
	Delta[r]=0x7FFF ;
      } ;
      break ;
    case STOP_PROG:
      if (Message.data[1]>PWM_CHANNELS) {
	for (r=0;r<PWM_CHANNELS;r++) {
	  Step[r] = 22 ;
	  Counter[r] = 0 ;
	  LEDVal[r] = 0 ;
	  LEDTargetVal[r] = 0 ;
	  LEDV2[r] = 0 ;
	} ;
      } else {
	r = Message.data[1]-1 ;
	Step[r] = 22 ;
	Counter[r] = 0 ; 
	LEDVal[r] = 0 ;
	LEDTargetVal[r] = 0 ;
	LEDV2[r] = 0 ;
      } ;
      break ;
    case DIM_TO:
      DimLED (LED,Message.data[2],Message.data[3],Message.data[4],Message.data[5],Message.data[6]) ;
      break ;
    case HDIM_TO:
      hsv_to_rgb(Message.data[2],Message.data[3],Message.data[4],&r,&g,&b) ;
      DimLED (LED,r,g,b,Message.data[5],Message.data[6]) ;
      break ;
    case SET_DIM_TIME:
      GlobalDimTimer = Message.data[1] ;
      break ;
    case SHADE_UP_FULL:
    case SHADE_DOWN_FULL:
    case SHADE_UP_SHORT:
    case SHADE_DOWN_SHORT:
    default:
      break ;
    } ;
  } ;
}
