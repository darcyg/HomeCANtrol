/*
  Sensor �ber CAN; Applikationsprogramm f�r den Bootloader.

  ATtiny84 @ 8 MHz
*/
 
 
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
#include "../Common/utils.h"

#undef cli
#undef sei

#define cli() __asm volatile( "cli" ::: "memory" )
#define sei() __asm volatile( "sei" ::: "memory" )

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
   9   n/a

   EEProm-Belegung vom Sensor:
   10 : Pin 1 Short tic with timer running
   10 : Board Address Low 
   11 : Board Address High
   12 : Line address
   13 : Command
   14 : Data 1    or Add1  if Command = 33
   15 : Data 2    or Port1 if Command = 33
   16 : Data 3    or Add2  if Command = 33
   17 : Data 4    or Port2 if Command = 33
   18 : Data 5    or Add3  if Command = 33
   19 : Data 6    or Port3 if Command = 33


   20 : Pin 1 Long Tic with timer running
   or: 20 Add4  if Command=33
       21 Port4 if Command=33
       22 Add5  if Command=33
       23 Port5 if Command=33
       24 Add6  if Command=33
       25 Port6 if Command=33
       26 Add7  if Command=33
       27 Port7 if Command=33
       28 Add8  if Command=33
       29 Port8 if Command=33
		 
   30 : Pin 1 Short Tic without timer running
   40 : Pin 1 Long Tic without timer running

   50-89   : Pin 2
   90-129  : Pin 3
   130-169 : Pin 4
   170-209 : Pin 5
   210-249 : Pin 6

   300 : Configuration Pin 1
   301 : Additional Data Pin 1
   302 : Configuration Pin 2
   303 : Additional Data Pin 2
   304 : Configuration Pin 3
   305 : Additional Data Pin 3
   306 : Configuration Pin 4
   307 : Additional Data Pin 4
   308 : Configuration Pin 5
   309 : Additional Data Pin 5
   310 : Configuration Pin 6
   311 : Additional Data Pin 6
   312 : REPEAT_START
   313 : REPEAT_NEXT

   Configuration: 1-9: Input-Funktionen
   1: Einfacher Input (Short Input)
   2: Short-Long-Input
   3: Digital-Input mit Timer (Monoflop)
   4: Digital-Input mit Timer (retriggerbar)
   5: Analog-Input
   6: Bewegungsmelder
   7: OC_Bewegungsmelder 
   8: Licht  - SDA liegt auf A0, SCL liegt auf A1

   10-19: Digital Output-Funktionen
   10: Ein-Aus
   11: PWM
   20-29: Kommunikationsfunktion
   20: WS2801 Clock
   21: WS2801 Data
*/
#define I_SIMPLE    1
#define I_SHORTLONG 2
#define I_MONO      3
#define I_RETRIG    4
#define I_ANALOG    5
#define I_BWM       6
#define I_BWM2      7
#define I_LIGHT     8

#define O_ONOFF    10
#define O_PWM      11
#define O_WSCLOCK  20
#define O_WSDATA   21 

#define MAX_LEDS 20

#define TIMER_PRESET 177

#define TIMEOUT 1000 // 10 Sekunden Timeout

// globale Variablen

can_t    Message ;
uint8_t  BoardLine ;
uint16_t BoardAdd ;

uint8_t  LastCommand ;
volatile uint16_t  Heartbeat ;
volatile uint8_t Time ;
volatile uint16_t Timers[6] ;
uint8_t  TimerStatus ;
uint8_t  Type[6] ;
uint8_t  Config[6];
uint8_t  Running[6] ;

uint8_t  PWMStep ;
volatile uint8_t  PWM[6] ; // gets modified in main
uint8_t  PWMTime[7] ;
uint8_t  PWMOut[6] ;

uint8_t  PWMTime2[7] ;
uint8_t  PWMOut2[6] ;

volatile uint8_t PWMSync ;

uint8_t* PWMPort[6] ;
uint8_t  PWMPin[7] ;
uint8_t SOLL_PWM[6] ; // gets modified in main, secure it with a memory barrier
uint8_t START_PWM[6] ; // gets modified in main
uint16_t TimerPWM[6] ; // gets modified in main
uint16_t DurationPWM[6] ; // gets modified in main

uint8_t  SOLL_WS[MAX_LEDS*3] ; // gets modified in main
uint8_t  START_WS[MAX_LEDS*3] ; // gets modified in main
uint16_t TimerLED[MAX_LEDS] ; // gets modified in main
uint16_t DurationLED[MAX_LEDS] ; // gets modified in main
uint8_t ChangedLED ;
volatile uint8_t  NumLED ; // gets modified in main

volatile uint8_t* PIX_Clock ;
volatile uint8_t* PIX_Data ;
uint8_t  PIX_CL ;
uint8_t  PIX_DA ;

volatile uint8_t  REPEAT_MASK ;
volatile uint16_t REPEAT_START ;
volatile uint16_t REPEAT_NEXT ;
volatile uint8_t  key_state;                               // debounced and inverted key state:
                                                           // bit = 1: key pressed
volatile uint8_t  key_press;                               // key press detect
volatile uint8_t  key_rpt;                                 // key long press and repeat


// BuildCANId baut aus verschiedenen Elementen (Line & Addresse von Quelle und Ziel 
// sowie Repeat-Flag und Gruppen-Flag) den CAN Identifier auf

uint32_t BuildCANId (uint8_t Prio, uint8_t Repeat, uint8_t FromLine, uint16_t FromAdd, uint8_t ToLine, uint16_t ToAdd, uint8_t Group)
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
  *FromAdd = (uint16_t) ((CANId>>14)&0xff) ;
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

// Message f�r das zur�cksenden vorbereiten (Quelle als Ziel eintragen und 
// Boardaddresse als Quelle)

void SetOutMessage (uint8_t BoardLine,uint16_t BoardAdd)
{
  uint8_t SendLine ;
  uint16_t SendAdd ;
  
  GetSourceAddress(Message.id,&SendLine,&SendAdd) ;
  Message.id = BuildCANId (0,0,BoardLine,BoardAdd,SendLine,SendAdd,0) ;
  Message.data[0] = Message.data[0]|SUCCESSFULL_RESPONSE ;
  Message.length = 1 ;
}

void SendPinMessage (uint8_t Pin, uint8_t Long,uint8_t SendData)
{
  uint8_t *Data ;
  uint16_t SendAdd ;
  uint8_t SendLine ;
  uint8_t Command ;
  uint8_t i ;
  
  Data = (uint8_t*)10 ;
  Data += Pin*40 ;
  if (Heartbeat>TIMEOUT) Data +=20 ;

  if (!Running[(int)Pin]) return ; // Dieser Pin ist ausgeschaltet worden

  Command = eeprom_read_byte(Data+3) ;
  SendLine = eeprom_read_byte(Data+2) ;

  if ((Command==(uint8_t)SHADE_UP_FULL)||(Command==(uint8_t)SHADE_DOWN_FULL)) {
    for (i=0;i<8;i++) {
      SendAdd = eeprom_read_byte(Data+4+(i<<1)) ;
      if (!SendAdd) break ; // Keine weiteren Empfaenger

      Message.id = BuildCANId(0,0,BoardLine,BoardAdd,SendLine,SendAdd,0) ;

      if (Command==(uint8_t)SHADE_UP_FULL) {	
	Message.data[0] = (Long==(uint8_t)1)?SHADE_UP_FULL:SHADE_UP_SHORT ;
      } else {
	Message.data[0] = (Long==(uint8_t)1)?SHADE_DOWN_FULL:SHADE_DOWN_SHORT ;
      }

      Message.data[1] = eeprom_read_byte(Data+5+(i<<1)) ;
      Message.length = 2 ;
      mcp2515_send_message(&Message) ;
    } ;
  } else {
    if (Long ==(uint8_t) 1) Data += 10 ;
	Command = eeprom_read_byte(Data+3) ;
	SendLine = eeprom_read_byte(Data+2) ;
    
    SendAdd = eeprom_read_byte(Data) ;
    SendAdd += ((uint16_t)eeprom_read_byte(Data+1))<<8 ;
    
    if (!(SendAdd||SendLine)) return ;
    
    Message.id = BuildCANId (0,0,BoardLine,BoardAdd,SendLine,SendAdd,0) ;
    Message.data[0] = Command ;
    Message.data[1] = (SendData==(uint8_t)0)?eeprom_read_byte(Data+4):SendData ;
    Message.data[2] = eeprom_read_byte(Data+5) ;
    Message.data[3] = eeprom_read_byte(Data+6) ;
    Message.data[4] = eeprom_read_byte(Data+7) ;
    Message.data[5] = eeprom_read_byte(Data+8) ;
    Message.data[6] = eeprom_read_byte(Data+9) ;
    Message.data[7] = Heartbeat>90?Time+1:Time ;
    Message.length = 8 ;
    mcp2515_send_message(&Message) ;
  } ;
}

void ws2801_writeByte(uint8_t Send)
{
  register uint8_t BitCount = 8; // store variable BitCount in a cpu register
  do {
    PIX_Clock[0] &= ~(PIX_CL);	// set clock LOW
    // send bit to ws2801. we do MSB first
    if (Send & 0x80) {
      PIX_Data[0] |= (PIX_DA); // set output HIGH
    } else {
      PIX_Data[0] &= ~(PIX_DA); // set output LOW
    } ;
    PIX_Clock[0] |= (PIX_CL); // set clock HIGH
    // next bit
    Send <<= 1;
  } while (--BitCount);
} // ws2801_writeByte


// Sortieren der PWM-Tabelle und bestimmen der Einschaltzeiten
inline void UpdatePWM (void)
{
  uint8_t i,j,k ;
  
  if (PWMSync) return ; // Has not been send out yet
  
  for (i=0;i<(uint8_t)6;i++) { PWMTime[i] = (uint8_t)255 ; PWMOut[i] = (uint8_t)0 ; } ;
  for (i=0;i<(uint8_t)6;i++) { // Alle durchgehen
    if (TimerPWM[i]>0) { 
      TimerPWM[i]-- ;
      PWM[i] = (uint8_t)((int16_t)START_PWM[i]+(int16_t)(((int32_t)SOLL_PWM[i]-(int32_t)START_PWM[i])*
							 ((int32_t)DurationPWM[i]-(int32_t)TimerPWM[i])/(int32_t)DurationPWM[i])) ;
      if (PWM[i]<(uint8_t)4) PWM[i]=(uint8_t)0 ;  // Don't exceed current driver frequency limitations
      if (PWM[i]>(uint8_t)251) PWM[i]=(uint8_t)255 ;
    } ;
    for (j=0;j<i;j++) if (PWM[i]<=PWMTime[j]) break ;
    for (k=(uint8_t)5;k>j;k--) { PWMTime[k] = PWMTime[k-1] ; PWMOut[k] = PWMOut[k-1] ; } ;
    PWMTime[j] = PWM[i] ;
    PWMOut[j] = i ;
  } ;
  for (i=(uint8_t)5,k=(uint8_t)0;i>0;i--) k+= (PWMTime[i]-=PWMTime[i-1]) ;
  PWMTime[6] = (uint8_t)255-k-PWMTime[0] ;
  PWMSync = 1 ;
}


// Haupt-Timer-Interrupt, wird alle 10 ms aufgerufen
// Setzt den Heartbeat, die Timer, das Dimmen der WS2801 und sorgt fuer das Tastenentprellen
// der an (a0:a3) und (b0:b3) angeschlossenen Tasten (wenn entsprechend konfiguriert)


ISR( TIM0_OVF_vect,ISR_NOBLOCK )                           
{
  static uint8_t ct0, ct1;
  static uint16_t rpt;
  static uint8_t WSCounter ;
  uint8_t WSByte ;
  uint8_t i,j,k;
 
  TCNT0 = (uint8_t)TIMER_PRESET;  // preload for 10ms

  TIMSK0 &= ~(1<<TOIE0);            // disable timer 0 interrupt

  if (Heartbeat<=TIMEOUT+1) Heartbeat++ ;
  for (i=0;i<6;i++) if (Timers[i]) Timers[i]-- ;

  WSCounter++ ;
  if (WSCounter>(uint8_t)1) {
    // Nur alle 20 ms updaten (50 Hz Update-Rate reicht); maximale Fading-Zeit liegt bei 25,5 Sekunden mit 0,1 Sekunde Aufloesung
    UpdatePWM () ;

    WSCounter = 0 ;
    // Berechnen des Sollwerts und Ausgeben desselben
    for (i=0;i<NumLED;i++) if (TimerLED[i]>0) break ;
    
    if (i<NumLED) {
      cli () ;
      for (i=0,k=0,j=0;i<NumLED*3;i++) {
	if (k==3) {
	  k = 0 ;
	  j++ ;
	}  ;
	if (!k) {
	  if (TimerLED[j]>0) {
	    TimerLED[j]-- ;
	  } ;
	} ;
	k++ ;
	WSByte = (uint8_t)((int16_t)START_WS[i]+(int16_t)(((int32_t)SOLL_WS[i]-(int32_t)START_WS[i])*
							  ((int32_t)DurationLED[j]-(int32_t)TimerLED[j])/(int32_t)DurationLED[j])) ;
	ws2801_writeByte(WSByte) ;
	if (!TimerLED[j]) {
	  DurationLED[j] = 1 ;
	  START_WS[i] = SOLL_WS[i] ;
	} ;
	ChangedLED=1 ;
      } ;
      PIX_Clock[0] &= (uint8_t)~(PIX_CL) ; //Clock Low zum Latchen
      sei () ;
    } else {
      if (ChangedLED){
	ChangedLED=0 ;
	// Noch einmal den letzten Wert ausgeben, damit der Wert �bernommen wird (das Pixel �bernimmt erst mit
	// Beginn des n�chsten Frames die Werte in die Ausgabe.
	cli () ;
	for (i=0;i<NumLED*3;i++) {
	  ws2801_writeByte(START_WS[i]) ;
	} ;
	PIX_Clock[0] &= (uint8_t)~(PIX_CL) ; //Clock Low zum Latchen
	sei () ;
      } ;
    } ;
  } ;
  
  i = key_state ^ ((((uint8_t)PINB&(uint8_t)0x7)<<3)|((uint8_t)PINA&(uint8_t)0x7));      // key changed ?
  ct0 = ~( ct0 & i );                             // reset or count ct0
  ct1 = ct0 ^ (ct1 & i);                          // reset or count ct1
  i &= ct0 & ct1;                                 // count until roll over ?
  key_state ^= i;                                 // then toggle debounced state
  key_press |= key_state & i;                     // 0->1: key press detect
 
  if(!(key_state & REPEAT_MASK))            // check repeat function
    rpt = REPEAT_START;                          // start delay
  if(!( --rpt)){
    rpt = REPEAT_NEXT;                            // repeat delay
    key_rpt |= key_state & REPEAT_MASK;
  }

  TIMSK0 |= 1<<TOIE0;            // enable timer 0 interrupt
  // Interrupts will be enabled by the rti at the end of the function
}
 
// check if a key has been pressed. Each pressed key is reported
// only once
uint8_t get_key_stat (uint8_t key_mask)
{
  return ((key_mask&(((PINB&(uint8_t)0x7)<<3)|(PINA&(uint8_t)0x7)))!=0);
}


uint8_t get_key_press( uint8_t key_mask )
{
  cli();                                          // read and clear atomic !
  key_mask &= key_press;                          // read key(s)
  key_press ^= key_mask;                          // clear key(s)
  sei();
  return key_mask;
}
 
// check if a key has been pressed long enough such that the
// key repeat functionality kicks in. After a small setup delay
// the key is reported being pressed in subsequent calls
// to this function. This simulates the user repeatedly
// pressing and releasing the key.

uint8_t get_key_rpt( uint8_t key_mask )
{
  cli();                                          // read and clear atomic !
  key_mask &= key_rpt;                            // read key(s)
  key_rpt ^= key_mask;                            // clear key(s)
  sei();
  return key_mask;
}
 
uint8_t get_key_short( uint8_t key_mask )
{
  cli();                                          // read key state and key press atomic !
  return get_key_press( ~key_state & key_mask );
  sei();
}
 
uint8_t get_key_long( uint8_t key_mask )
{
  return get_key_press( get_key_rpt( key_mask ));
}
    
// Anschalten des angegebenen Ports

inline void PortOn(uint8_t Port)
{
  if (PWMPin[Port]) PWMPort[Port][0]|=PWMPin[Port] ;
}

// Ausschalten des angegebenen Ports

inline void PortOff(uint8_t Port)
{
  if (PWMPin[Port]) PWMPort[Port][0]&=(uint8_t)~(PWMPin[Port]) ;
}

inline void SwapPWM(void)
{
  /*  for (i=0;i<6;i++){
    PWMTime2[i]=PWMTime[i] ;
    PWMOut2[i]=PWMOut[i] ;
    } ; */
  PWMOut2[0]=PWMOut[0] ;
  PWMOut2[1]=PWMOut[1] ;
  PWMOut2[2]=PWMOut[2] ;
  PWMOut2[3]=PWMOut[3] ;
  PWMOut2[4]=PWMOut[4] ;
  PWMOut2[5]=PWMOut[5] ;
  PWMTime2[0]=PWMTime[0];
  PWMTime2[1]=PWMTime[1];
  PWMTime2[2]=PWMTime[2];
  PWMTime2[3]=PWMTime[3];
  PWMTime2[4]=PWMTime[4];
  PWMTime2[5]=PWMTime[5];
  PWMTime2[6]=PWMTime[6];
}

// Timer1-Interrup-Service-Routine der PWM-Generierung

ISR ( TIM1_OVF_vect )     
{
  uint8_t i ;
  
  if (!PWMStep) {
    if (PWMSync) {
      SwapPWM () ;
      PWMSync=0 ;
    }
    for (;(PWMTime2[PWMStep]==0)&&(PWMStep<7);PWMStep++) ;
    TCNT1=65535-(PWMTime2[PWMStep]<<2) ;
    for (i=PWMStep;i<6;i++) PortOn(PWMOut2[i]) ;
    PWMStep++ ;
  } else {
    do {
      PortOff(PWMOut2[PWMStep-1]) ;
      PWMStep++ ;
    } while ((PWMTime2[PWMStep-1]==(uint8_t)0)&&(PWMStep<7)) ;
    TCNT1=65535-(PWMTime2[PWMStep-1]<<2) ;
    for (i=PWMStep;i<7;i++) if (PWMTime2[i]) break ; // If no further shut off follows, set to PWMStep7
  } ;
  if ((i==(uint8_t)7)||(PWMStep==(uint8_t)7)) {
    PWMStep = 0 ;
  } ;
}

// Initialisieren des MC und setzen der Port-Eigenschaften
// in Abhaengigkeit von der Konfiguration

uint8_t Bits[]={1,2,4,8,16,32} ;

void InitMC (void)
{
  uint8_t i ;

    
  // Timer 1 OCRA1, als variablem Timer nutzen
  TCCR1B = 3|8;             // Timer laeuft mit Prescaler/64 
  TCNT1 = 65530;              // Timer auf Null stellen
  TIMSK1 |= (1 << TOIE1);   // Interrupt freischalten

  // Timer 0 als 10 ms-Timer verwenden
  TCCR0B = (1<<CS02)|(1<<CS00);  // divide by 1024
  TCNT0 = (uint8_t)TIMER_PRESET; // preload for 10ms
  TIMSK0 |= 1<<TOIE0;            // enable timer interrupt
  
  // Port-Konfiguration durchgehen
  PWMPin[6]=0 ;
  PWMTime2[6]=255 ;

  for (i=0;i<6;i++) {
    // Konfigurations-Byte lesen
    Type[i] = eeprom_read_byte((uint8_t*)(300+(i<<1))) ;
    Config[i] = eeprom_read_byte((uint8_t*)(301+(i<<1))) ;
    PWM[i]= 0 ;

    switch (Type[i]) {
    case I_SIMPLE: // Klick-Input
    case I_SHORTLONG: // Short-Long-Input
    case I_MONO: // Monoflop
    case I_RETRIG: // Retriggerbares Monoflop
      // Einfacher Input
      if (Type[i]==(uint8_t)I_SHORTLONG) {
	REPEAT_START = eeprom_read_byte((uint8_t*)312)*10 ; // in 1/10 Sekunden
	REPEAT_NEXT  = eeprom_read_byte((uint8_t*)313)*10 ;  // in 1/10 Sekunden
	REPEAT_MASK |= Bits[i] ;
      } ;
      if (i<(uint8_t)3) {
	// Port A
	PORTA &= (uint8_t)~Bits[i] ;
	DDRA &= (uint8_t)~Bits[i] ;
      } else {
	// Port B
	PORTB &= (uint8_t)~Bits[i-3] ;
	DDRB &= (uint8_t)~Bits[i-3] ;
      } ;
      break ;
    case I_ANALOG: // Analog-Input
      // ADC Konverter initialisieren; es kann immer nur ein Port ADC-Port sein, hier wird jedoch
      // dies nicht abgefragt -> der letzte angegebene Port ist ADC port
      // Port kann nur in Port A liegen
      if (i>(uint8_t)2) break ;
      PORTA &= (uint8_t)~Bits[i] ;
      DDRA &= (uint8_t)~Bits[i] ;
      ADMUX = i ; // VCC Reference voltage, PortA0-PortA2 als Eingang
      ADCSRB = 1<<4 ; // Right adjusted, Unipolar, No comparator, Free-Running
      ADCSRA = (1<<7)||(1<<6)||(1<<5)||(1<<2)||(1<<1)||(1<<0) ; // ADC Enable, ADC On, ADC FreeRun, Clock/128
      break ;
    case O_ONOFF: // Ein-Aus
    case O_PWM: // PWM
    case O_WSCLOCK: // WS2801 Clock
    case O_WSDATA: // WS2801 Data
      // Ausgabe-Port (Ein-Aus oder PWM), wird entsprechend dem Config Byte initalisiert
      if (Type[i]==(uint8_t)O_WSCLOCK) { // WS Clock: Pointer auf den richtigen Port-Pin setzen
	if (i<3) { 
	  PIX_Clock = (uint8_t*)&PORTA ;
	  PIX_CL = Bits[i] ;
	} else {
	  PIX_Clock = (uint8_t*)&PORTB ;
	  PIX_CL = Bits[i-3] ;
	} ;
      } ;
      if (Type[i]==(uint8_t)O_WSDATA) { // WS Data: Pointer auf den richtigen Port-Pin setzen
	if (i<3) { 
	  PIX_Data = (uint8_t*)&PORTA ;
	  PIX_DA = Bits[i] ;
	} else {
	  PIX_Data = (uint8_t*)&PORTB ;
	  PIX_DA = Bits[i-3] ;
	} ;
      } ;
      if (i<3) {
	// Port A
	PWMPort[i] = (uint8_t*)&PORTA ;
	if (Type[i]==(uint8_t)O_PWM){
	  PWMPin[i] = Bits[i] ;
	} else {
	  if (Config[i]) {
	    PORTA |= Bits[i] ;
	  } else {
	    PORTA &= (uint8_t)~Bits[i] ;
	  } ;
	  PWMPin[i] = 0 ;
	} ;
	DDRA |= Bits[i] ;
      } else {
	// Port B
	PWMPort[i] = (uint8_t*)&PORTB ;
	if (Type[i]==(uint8_t)O_PWM){
	  PWMPin[i] = Bits[i-3] ;
	} else {
	  if (Config[i]) {
	    PORTB &= (uint8_t)~Bits[i-3] ;
	  } else {
	    PORTB |= Bits[i-3] ;
	  } ;
	  PWMPin[i] = 0 ;
	} ;
	DDRB |= Bits[i-3] ;
      } ;
      break ;
    } ;
  } ;	  
}	



// Hauptprogramm
 
int __attribute__((OS_main)) main(void) 
{
  uint8_t r ;
  uint8_t i,j ;
  uint16_t Addr ;
  
  // Default-Werte:
  BoardAdd = 16 ;
  BoardLine = 1 ;
 

  // Lesen der EEProm-Konfiguration
  
  r = eeprom_read_byte((uint8_t*)0) ;
  if (r==(uint8_t)0xba) {
    r = eeprom_read_byte((uint8_t*)1) ;
    if (r==(uint8_t)0xca) {
      BoardAdd = eeprom_read_byte((uint8_t*)2) ;
      BoardAdd += ((uint16_t)eeprom_read_byte((uint8_t*)3))<<8 ;
      BoardLine = eeprom_read_byte((uint8_t*)4) ;
    } ;
  } ;


  // Initialisieren des CAN-Controllers
  mcp2515_init();
  
  /* Filter muss hier gesetzt werden */	
  SetFilter(BoardLine,BoardAdd) ;

  for (r=0;r<6;r++) Running[r] = 1 ; // Alle Eing�nge sind aktiv

  // Ports, Timer, ADC initialisieren
  InitMC() ;
 
  sei();                  // Interrupts gloabl einschalten

  // Say Hello, world...
  
  Message.id = BuildCANId (0,0,BoardLine,BoardAdd,0,1,0) ;
  Message.data[0] = IDENTIFY ;
  Message.length = 1 ;
  mcp2515_send_message(&Message) ;

  // Endlosschleife zur Abarbeitung der Kommandos


  while(1) {

    // Warte auf die n�chste CAN-Message
    while ((LastCommand=mcp2515_get_message(&Message)) == (uint8_t)NO_MESSAGE) {
      /* Ports verarbeiten */
      for (i=0,j=1;i<6;i++,j=j<<1) {
	// Aktion je nach Konfigurations-Byte ausfuehren
	switch (Type[i]) {
	case I_SIMPLE: // Einfacher Eingang
	  if (get_key_press(j)) {
	    SendPinMessage(i,0,0) ;
	  } ;
	  break ;
	case I_SHORTLONG: // Kurz oder lang gedrueckt
	  if (get_key_short(j)) {
	    SendPinMessage(i,0,0) ;
	  } else if (get_key_long(j)) {
	    SendPinMessage(i,1,0) ;
	  } ;
	  break ;
	case I_MONO: // Nicht-Nachstellbares Monoflop
	  if (!Timers[i]) {
	    if (get_key_press(j)) {
	      SendPinMessage(i,0,0) ;
	      Timers[i] = ((uint16_t)Config[i])*100 ;
	      TimerStatus |= j ;
	    } else {
	      if (TimerStatus&j) {
     		SendPinMessage(i,1,0) ;
	    	TimerStatus &= (uint8_t)~(j) ;
	      } ;
	    } ;
	  } ;
	  break ;
	case I_RETRIG: // Nachstellbares Monoflop
	  if (get_key_press(j)) {
	    if (!Timers[i]) {
	      SendPinMessage(i,0,0) ;
	    } ;
	    Timers[i] = ((uint16_t)Config[i])*100 ;
	    TimerStatus |= j ;
	  } ;
	  if (!Timers[i]) {
	    if (TimerStatus&j) {
	      SendPinMessage(i,1,0) ;
	      TimerStatus &= (uint8_t)~(j) ;
	    } ;
	  } ;
	  break ;
  	case I_BWM: // Nachstellbares Monoflop als Bewegungsmelder
	case I_BWM2:
	  r=!get_key_stat(j) ;
	  if (Type[i]==(uint8_t)I_BWM2) r=!r ;
	  if (r) {
	    if (!Timers[i]) {
	      SendPinMessage(i,0,0) ;
	    } ;
	    Timers[i] = ((uint16_t)Config[i])*100 ;
	    TimerStatus |= j ;
	  } ;
	  if (!Timers[i]) {
	    if (TimerStatus&j) {
	      SendPinMessage(i,1,0) ;
	      TimerStatus &= (uint8_t)~(j) ;
	    } ;
	  } ;
	  break ;

	case I_ANALOG: // Analog-Input, wird alle ConfigByte-Sekunden auf dem Bus ausgegeben.
	  if (!Timers[i]) {
	    r = ADCH ;
	    SendPinMessage(i,0,r) ;
	    Timers[i] = ((uint16_t)Config[i])*100 ;
	  } ;
	} ;
      } ;
    };
    
    // Kommando extrahieren
    r = Message.data[0] ;
    j = Message.data[1] ;

    // Sende-Addresse zusammenstoepseln (enth�lt auch die Quelladdresse aus Message,
    // ueberschreibt dann die In-Message)
    SetOutMessage(BoardLine,BoardAdd) ;

    // Befehl abarbeiten
    switch (r) {
      
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
      Heartbeat = 0 ;
      Time = j ;
      break ;
      // Diese Befehle sind beim Sensor nicht bekannt
      // LED
      // Relais-Befehle
    case CHANNEL_ON:
    case CHANNEL_OFF:
    case CHANNEL_TOGGLE:
      j-- ;
      if (j>(uint8_t)5) j=0 ;
      for (;(j<Message.data[1])&&(j<6);j++) { // Wenn Port = 1..6, dann nur diesen, sonst alle
	if ((Type[j]!=(uint8_t)O_ONOFF)&&(Type[j]!=(uint8_t)O_PWM)) continue ; // Illegaler PIN
	if (r==(uint8_t)CHANNEL_ON) {
	  i = 255 ;
	} else if (r==(uint8_t)CHANNEL_OFF) {
	  i = 0 ;
	} else {
	  i =255-PWM[j] ;
	}
	cli (); 
	START_PWM[j] = PWM[j] ;
	SOLL_PWM[j] = i ;
	if (Config[j]) {
	  TimerPWM[j] = DurationPWM[j] = Config[j] ;
	} else {
	  TimerPWM[j] = DurationPWM[j] = 1 ;
	} ;
	sei () ;
      } ;
      break ;
      // Nun die Sensor-Befehle
    case SET_PIN:
    case DIM_TO:
      if (r==DIM_TO) {
	r = 3 ;
      } else {
	r = 0 ;
      } ;
      j-- ;
      if (j>(uint8_t)5) j=0 ;
      for (;(j<Message.data[1])&&(j<6);j++) { // Wenn Port = 1..6, dann nur diesen, sonst alle
	if ((Type[j]!=O_ONOFF)&&(Type[j]!=O_PWM)) break ; // Illegaler PIN
	cli () ;
	START_PWM[j] = PWM[j] ;
	SOLL_PWM[j] = Message.data[2+r] ;
	TimerPWM[j] = DurationPWM[j] = (Message.data[3+r]<<2)+1 ;
	sei () ;
      } ;
      break ;
    case LOAD_LED:
      if ((j+1)>(uint8_t)MAX_LEDS) break; // Zu hohe LED-Nummer
      cli () ;
      NumLED = (j+1)>NumLED?(j+1):NumLED ; // Set Max used LED
      START_WS[j*3] = SOLL_WS[j*3] ;
      START_WS[j*3+1] = SOLL_WS[j*3+1] ;
      START_WS[j*3+2] = SOLL_WS[j*3+2] ;
      SOLL_WS[j*3] = Message.data[2] ;
      SOLL_WS[j*3+1] = Message.data[3] ;
      SOLL_WS[j*3+2] = Message.data[4] ;
      DurationLED[j] = TimerLED[j] = (Message.data[5]<<2)+1 ;
      sei () ;
      break ;
    case START_SENSOR:
      Running[(int)j-1] = 1 ;
      break ;
    case STOP_SENSOR:
      Running[(int)j-1] = 0 ;
      break ;
    default:
      break ;
    } ;
  } ;
}
