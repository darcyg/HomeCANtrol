/*****************************************************
 */


#include <stdio.h>
#include "stm32f10x.h"
#include "ws2812.h"

// Buffer for LED
rgb_t 	WSRGB[MAXWSNUM];	
int WSDimmer ;
int CurrentWSNum ;

static uint16_t 		wstimerVals[WS_DMA_LEN+1];	// buffer for timer/dma, one byte per bit + reset pulse
volatile uint8_t		ledBusy = 0;							// = 1 while dma is sending data to leds




static void WSstartDMA(void);

//-------------------------------------------------------------------------------------------------------------

static uint16_t *rgb2pwm(uint16_t *buf, const uint8_t color)
{
  register uint8_t mask = 0x80;
  
  do {
    if (color & mask) {
      *buf = WS_ONE;
    } else {
      *buf = WS_ZERO;
    } ;
    buf++;
    mask >>= 1;
  } while (mask);
  
  return buf;
}

void WSupdate(void) 
{
  register uint32_t i;
  register rgb_t *r;
  uint16_t *bufp = wstimerVals;
  int c;
  
  for (i = 0; i < ledsPhysical; i++) {
    r = (rgb_t *)&WSRGB[i];
    c = ((int)r->G * WSDimmer) / 100;
    bufp = rgb2pwm(bufp, (const uint8_t)c);
    c = ((int)r->R * WSDimmer) / 100;
    bufp = rgb2pwm(bufp, (const uint8_t)c);
    c = ((int)r->B * WSDimmer) / 100;
    bufp = rgb2pwm(bufp, (const uint8_t)c);
  }

  // append reset pulse (50us low level)
  for (i = 0; i < WS_RESET_LEN; i++) *bufp++ = 0;
  
  WSstartDMA();		// send it to RGB stripe
}





void WSinit(void)
{
  GPIO_InitTypeDef GPIO_InitStructure;
  TIM_TimeBaseInitTypeDef timbaseinit;
  TIM_OCInitTypeDef timocinit;
  NVIC_InitTypeDef nvic_init;
  int i;

  // clear buffer

  for (i = 0; i < (WS_DMA_LEN - WS_RESET_LEN); i++) wstimerVals[i] = WS_ZERO;
  
  for (; i < WS_DMA_LEN; i++) wstimerVals[i] = 0;
  
  for (i = 0; i < LEDS_MAXTOTAL; i++) {
    WSRGB[i].B = 0;
    WSRGB[i].G = 0;
    WSRGB[i].R = 0;
  } ;
  
  // GPIO
  RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
  
  GPIO_InitStructure.GPIO_Pin = GPIO_Pin_8;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
  GPIO_Init(GPIOA, &GPIO_InitStructure);
  
  // Timer
  RCC_APB2PeriphClockCmd(RCC_APB2Periph_TIM1, ENABLE);
  
  TIM_TimeBaseStructInit(&timbaseinit);
  timbaseinit.TIM_ClockDivision = TIM_CKD_DIV1;
  timbaseinit.TIM_CounterMode = TIM_CounterMode_Up;
  timbaseinit.TIM_Period = W_TIM_FREQ / WS_OUT_FREQ;
  timbaseinit.TIM_Prescaler = 1 ;
  TIM_TimeBaseInit(TIM1, &timbaseinit);
  
  TIM_OCStructInit(&timocinit);
  timocinit.TIM_OCMode = TIM_OCMode_PWM1;
  timocinit.TIM_OCPolarity = TIM_OCPolarity_High;
  timocinit.TIM_OutputState = TIM_OutputState_Enable;
  timocinit.TIM_Pulse = 0;
  TIM_OC1Init(TIM1, &timocinit);
  TIM_OC1PreloadConfig(TIM1, TIM_OCPreload_Enable);
  TIM_ARRPreloadConfig(TIM1, ENABLE);

  TIM_CCxCmd(TIM1, TIM_Channel_1, TIM_CCx_Enable);
  TIM_Cmd(TIM1, ENABLE);

  // DMA
  RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
  TIM_DMACmd(TIM1, TIM_DMA_CC1, ENABLE);
  DMA_ITConfig(DMA1_Channel2, DMA_IT_TC, ENABLE);

  // NVIC for DMA
  nvic_init.NVIC_IRQChannel = DMA1_Channel2_IRQn;
  nvic_init.NVIC_IRQChannelPreemptionPriority = 1;
  nvic_init.NVIC_IRQChannelSubPriority = 1;
  nvic_init.NVIC_IRQChannelCmd = ENABLE;
  NVIC_Init(&nvic_init);

  WSstartDMA();
}



static DMA_InitTypeDef dma_init = {
  .DMA_BufferSize = (WS2812_RESET_LEN),
  .DMA_DIR = DMA_DIR_PeripheralDST,
  .DMA_M2M = DMA_M2M_Disable ;
  .DMA_MemoryBaseAddr = (uint32_t) &ws2812timerValues[0],
  .DMA_MemoryDataSize	= DMA_MemoryDataSize_HalfWord,
  .DMA_MemoryInc = DMA_MemoryInc_Enable,
  .DMA_Mode = DMA_Mode_Normal,
  .DMA_PeripheralBaseAddr = (uint32_t) &(TIM1->CCR1),
  .DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord,
  .DMA_PeripheralInc = DMA_PeripheralInc_Disable,
  .DMA_Priority = DMA_Priority_Medium
};


// transfer framebuffer data to the timer
static void WSstartDMA(void)
{
  if (ledBusy)		// last DMA is not finished
    return;
  
  ledBusy = 1;
  dma_init.DMA_BufferSize = WS_RESET_LEN+CurrentWSNum*3*8 ;

  DMA_Cmd(DMA1_Channel2, DISABLE);
  DMA_Init(DMA1_Channel2, &dma_init);
  DMA_Cmd(DMA1_Channel2, ENABLE);
  TIM_DMACmd(TIM1, TIM_DMA_CC1, ENABLE);
}

// gets called when dma transfer has completed
void DMA1_Channel2_IRQHandler(void)
{
  DMA_ClearITPendingBit(DMA1_IT_TC2);
  DMA_Cmd(DMA1_Channel2, DISABLE);
  // need to disable this, otherwise some glitches can occur (first bit gets lost)
  TIM_DMACmd(TIM1, TIM_DMA_CC1, DISABLE);
  
  ledBusy = 0;			// get ready for next transfer
}




void WStest(void)
{
  uint32_t i,j;
#define NR_TEST_PATTERNS	12
  uint8_t patterns[NR_TEST_PATTERNS][3] = {
    {0xf0,0x00,0x00},
    {0x00,0xf0,0x00},
    {0x00,0x00,0xf0},
    {0xf0,0xf0,0x00},
    {0xf0,0x00,0xf0},
    {0x00,0xf0,0xf0},
    {0xf0,0xf0,0xf0},
    {0x40,0x40,0x40},
    {0xf0,0xf0,0xf0},
    {0x40,0x40,0x40},
    {0xf0,0xf0,0xf0},
    {0x00,0x00,0x00},
  };
  
  for (i = 0; i < ledsPhysical; i++) {
    ws2812ledRGB[i].R = 0;
    ws2812ledRGB[i].G = 0;
    ws2812ledRGB[i].B = 0;
  }
  
  for(j=0; j<NR_TEST_PATTERNS; j++)
    {
      for (i = 0; i < ledsPhysical; i++)
	{
	  ws2812ledRGB[i].R = patterns[j][0] * 0.5F;	// 50% brigthness; my DC power supply is weak and I got brown outs
	  ws2812ledRGB[i].G = patterns[j][1] * 0.5F;
	  ws2812ledRGB[i].B = patterns[j][2] * 0.5F;
	}
      while (ledBusy)
	;
      WS2812update();
      delay_ms(60);
    }
  
}