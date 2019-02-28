/* Copyright (c) 2015 Nordic Semiconductor. All Rights Reserved.
 *
 * The information contained herein is property of Nordic Semiconductor ASA.
 * Terms and conditions of usage are described in detail in NORDIC
 * SEMICONDUCTOR STANDARD SOFTWARE LICENSE AGREEMENT.
 *
 * Licensees are granted free, non-transferable use of the information. NO
 * WARRANTY of ANY KIND is provided. This heading must NOT be removed from
 * the file.
 *
 */

#include "sdk_config.h"
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "bsp.h"
#include "boards.h"
#include "nordic_common.h"
#include "nrf_drv_clock.h"
#include "nrf_drv_spi.h"
#include "nrf_uart.h"
#include "app_util_platform.h"
#include "nrf_gpio.h"
#include "nrf_delay.h"
#include "nrf_log.h"
#include "nrf.h"
#include "app_error.h"
#include "app_util_platform.h"
#include "app_error.h"
#include <string.h>
#include "port_platform.h"
#include "deca_types.h"
#include "deca_param_types.h"
#include "deca_regs.h"
#include "deca_device_api.h"
#include "nrf_drv_gpiote.h"
#include "UART.h"
#include "message_transceiver.h"
#include "int_handler.h"
#include "low_timer.h"

//-----------------dw1000----------------------------

static dwt_config_t config = {
    5,                /* Channel number. */
    DWT_PRF_64M,      /* Pulse repetition frequency. */
    DWT_PLEN_128,     /* Preamble length. Used in TX only. */
    DWT_PAC8,         /* Preamble acquisition chunk size. Used in RX only. */
    10,               /* TX preamble code. Used in TX only. */
    10,               /* RX preamble code. Used in RX only. */
    0,                /* 0 to use standard SFD, 1 to use non-standard SFD. */
    DWT_BR_6M8,       /* Data rate. */
    DWT_PHRMODE_STD,  /* PHY header mode. */
    (129 + 8 - 8)     /* SFD timeout (preamble length + 1 + SFD length - PAC size). Used in RX only. */
};

//--------------dw1000---end---------------

/* Macros definitions */
// Antenna delays
#define TX_ANT_DLY 16456
#define RX_ANT_DLY 16456
// Frames related
#define MSG_LEN 24 // Length (bytes) of the standard message
// Ranging related
#define NODE_ID 1 // Node ID
#define RANGE_FREQ 1 // Frequency of the cycles
#define TX_INTERVAL 70000 // In microseconds
#define N 4 /**< Number of nodes */
#define UUS_TO_DWT_TIME 65536 // Used to convert microseconds to DW1000 register time values.


#define TASK_DELAY 200           /**< Task delay. Delays a LED0 task for 200 ms */
#define TIMER_PERIOD 2000          /**< Timer period. LED1 timer will expire after 1000 ms */
#define TX_GAP 400 /**< Time interval between transmits, in microseconds */

/** Buffer for timestamps */
double timeBuf[3*N];
for (int i = 0; i < 3*N; i++) {
  timeBuf[i] = -1;
}

/** Buffer for distances */
double distanceBuf[N];
for (int i = 0; i < N; i++) {
  distanceBuf[i] = -1;
}

/** ID of node that requested from this node. */
double rxId;

#ifdef USE_FREERTOS

TaskHandle_t run_task_handle;   /**< Reference to SS TWR Initiator FreeRTOS task. */
extern void runTask (void * pvParameter);
TaskHandle_t led_toggle_task_handle;   /**< Reference to LED0 toggling FreeRTOS task. */
TimerHandle_t led_toggle_timer_handle;  /**< Reference to LED1 toggling FreeRTOS timer. */
#endif

/* Local function prototypes */
void runTask (void * pvParameter);
static void initTimerHandler(void *pContext);
static void sleepTimerHandler(void *pContext);
static void initCycleTimings(void);
static void initListen(void);
static void setTxDelay(int nodeId);
static void setFnDelay(int nodeId);

/* Global variables */
// Frames related
// msg[] is the entire frame to transmitted out. there is a frame format (first 10 bytes and last 2 bytes) to follow, check the dw1000 manual.
uint8 msg[MSG_LEN] = {0x41, 0x88, 0, 0xCA, 0xDE, 'W', 'A', 'V', 'E', 0xE0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
uint8 buf[MSG_LEN] = { 0 }; // enough size to hold all data from a received frame
uint32 cyclePeriod;
uint32 activePeriod;
uint32 sleepPeriod;
// States related
bool isInitiating = false;
bool isSleeping = false;
bool txSuccess = false;
// Timers related
APP_TIMER_DEF(initTimer);
APP_TIMER_DEF(sleepTimer);
int counter = 0; // debugging purpose

#ifdef USE_FREERTOS

  /**@brief LED0 task entry function.
   *
   * @param[in] pvParameter   Pointer that will be used as the parameter for the task.
   */
  static void led_toggle_task_function (void * pvParameter)
  {
    UNUSED_PARAMETER(pvParameter);
    while (true)
    {
      LEDS_INVERT(BSP_LED_0_MASK);
      // Delay a task for a given number of ticks
      vTaskDelay(TASK_DELAY);
      // Tasks must be implemented to never return...
    }
  }

  /**@brief The function to call when the LED1 FreeRTOS timer expires.
   *
   * @param[in] pvParameter   Pointer that will be used as the parameter for the timer.
   */
  static void led_toggle_timer_callback (void * pvParameter)
  {
    UNUSED_PARAMETER(pvParameter);
    LEDS_INVERT(BSP_LED_1_MASK);
  }
#endif   // #ifdef USE_FREERTOS

int main(void)
{
  // Setup some LEDs for debug Green and Blue on DWM1001-DEV
  LEDS_CONFIGURE(BSP_LED_0_MASK | BSP_LED_1_MASK | BSP_LED_2_MASK);
  LEDS_ON(BSP_LED_0_MASK | BSP_LED_1_MASK | BSP_LED_2_MASK );

  #ifdef USE_FREERTOS
    // Create task for LED0 blinking with priority set to 2
    UNUSED_VARIABLE(xTaskCreate(led_toggle_task_function, "LED0", configMINIMAL_STACK_SIZE + 200, NULL, 2, &led_toggle_task_handle));

    // Start timer for LED1 blinking
    led_toggle_timer_handle = xTimerCreate( "LED1", TIMER_PERIOD, pdTRUE, NULL, led_toggle_timer_callback);
    UNUSED_VARIABLE(xTimerStart(led_toggle_timer_handle, 0));

    // Create task for SS TWR Initiator set to 2
    UNUSED_VARIABLE(xTaskCreate(runTask, "entry task", configMINIMAL_STACK_SIZE + 200, NULL, 2, &run_task_handle));
  #endif // #ifdef USE_FREERTOS
  
  //-------------dw1000  ini------------------------------------	

  // Setup NRF52832 interrupt on GPIO 25 : connected to DW1000 IRQ
  vInterruptInit();
  
  // Initialization UART
  boUART_Init();
  printf("Initialising node\r\n");
  
  // Reset DW1000
  reset_DW1000(); 

  // Set SPI clock to 2MHz
  port_set_dw1000_slowrate();			
  
  // Init the DW1000
  if (dwt_initialise(DWT_LOADUCODE) == DWT_ERROR)
  {
    //Init of DW1000 Failed
    while (1) {};
  }

  // Configure DW1000.
  dwt_configure(&config);

  // Initialization of the DW1000 transceiver interrupts
  // Callback are defined in int_handler module
  dwt_setcallbacks(&tx_conf_cb, &rx_ok_cb, &rx_to_cb, &rx_err_cb);

  // Enable wanted interrupts (TX confirmation, RX good frames, RX timeouts and RX errors).
  dwt_setinterrupt(DWT_INT_TFRS | DWT_INT_RFCG | DWT_INT_RFTO | DWT_INT_RXPTO | DWT_INT_RPHE | DWT_INT_RFCE | DWT_INT_RFSL | DWT_INT_SFDT, 1);

  // Apply default antenna delay value
  dwt_setrxantennadelay(RX_ANT_DLY);
  dwt_settxantennadelay(TX_ANT_DLY);

  // Create the required timers
  lowTimerInit();
  lowTimerSingleCreate(&initTimer, initTimerHandler);
  lowTimerSingleCreate(&sleepTimer, sleepTimerHandler);

  // Pre-calculate all the timings in one cycle (ie, cycle, active, sleep period).
  initCycleTimings();

  //-------------dw1000  ini------end---------------------------	
  // IF WE GET HERE THEN THE LEDS WILL BLINK

  #ifdef USE_FREERTOS		
    // Start FreeRTOS scheduler.
    vTaskStartScheduler();	

    while(1) 
    {};
  #else
    int pvParameter = 0;
    runTask(&pvParameter);
  #endif
}

/**
 * @brief Node entry function.
 * 
 * @param[in] pvParameter Pointer that will be used as the parameter for the task.
 */
void runTask (void * pvParameter)
{
  UNUSED_PARAMETER(pvParameter);
  dwt_setleds(DWT_LEDS_ENABLE);

  // Listen for activity in the network
  initListen();
  while (isInitiating) {};
  printf("%d\r\n", counter); // debugging purpose

  while(true)
  {
    while (isSleeping) {};
    printf("1\r\n"); // debugging purpose

    txSuccess = false;
    setTxDelay(NODE_ID);
    if (NODE_ID != 1)
    {
      txMsg(msg, MSG_LEN, DWT_START_TX_DELAYED);
    }
    else
    {
      txMsg(msg, MSG_LEN, DWT_START_TX_IMMEDIATE);
    }

    while (!txSuccess) {};
    // ISSUE: If we comment the line below out, the cycle is able to complete.
    // Otherwise, its stuck at the next poll for txSuccess. And second message
    // fails to TX.
    // Why? Is it somehow causing the tx delay time set to be unsuccessful?
    // In SEGGER debugger, things run smoothly when stepping through but not
    // when running this code in real time. ???
    txSuccess = false;

    setFnDelay(NODE_ID);
    if (txMsg(msg, MSG_LEN, DWT_START_TX_DELAYED) == TX_SUCCESS)
    {
      printf("s\r\n"); // debugging purpose
    }

    while (!txSuccess) {};

    // Use initiate timer to simulate receiving of other nodes
    isInitiating = true;
    lowTimerStart(initTimer, (N - NODE_ID) * TX_INTERVAL);
    while (isInitiating) {};

    // Sleep from now on
    isSleeping = true;
    dwSleep(false);
    lowTimerStart(sleepTimer, sleepPeriod);
  }
}

void enterNetwork(int id) {
  nodeListen();
  nodeSleep();
  nodeLoop(id);
}

void nodeListen() {
  dwt_rxenable(DWT_START_RX_IMMEDIATE);
  nodeRxStore();
}

void nodeLoop(int id) {
  while(true) {
    nodeWakeUp();
    nodeProtocol(id);
    nodeSleep(id);
  }
}

void nodeListen() {

  /* Clear reception timeout to start next ranging process. */
  dwt_setrxtimeout(0);

  /* Activate reception immediately. */
  dwt_rxenable(DWT_START_RX_IMMEDIATE);

}

/**
 * @brief Stores received data in the correct buffer.
 * If received request, store received id, for subsequent transmission.
 *
 * Uses two global variables:
 * timeBuf
 * distanceBuf
 */
void nodeRxStore() {
  double rxBuf[3];
  MsgType msgType;

  rxMsg(rxBuf, &msgType);
  double id = rxBuf[0];
  double data1 = rxBuf[1]; // time1 / distance
  double data2 = rxBuf[2]; // time2 / distance
  double data3 = rxBuf[3]; // time3 / distance

  case (msgType) {
    switch MSG_TYPE_TIME:
      timeBuf[3*id] = data1;
      timeBuf[3*id+1] = data2;
      timeBuf[3*id+2] = data3;
      break;
    switch MSG_TYPE_DISTANCE:
      distanceBuf[id] = data1;
      break;
    switch MSG_TYPE_REQUEST:
      rxId = id;
      break;
  }

}

/**
 * @brief Protocol implementation
 *
 * @param[id] ID of node executing this protocol
 * If ID is 1, then broadcast immediately
 * Else, receive previous ID's transmission, then broadcast
 */
void nodeProtocol(int id) {
  uint8 rxBuf[32];

  // No time/dist, no timestamp, isRequest
  uint8 requestMsg[] = {0x41, 0x88, 0, 0xCA, 0xDE, 'W', 'A', 'V', 'E', 0xE0, id, 0, 0, 1};

  // Implementation
  for (int i = 1; i < id; i++) {
    nodeRxStore();
  }
  txMsg(requestMsg, 14, DWT_START_TX_IMMEDIATE);
  for (int i = 1; i < N; i++) {
    nodeRxStore();
  }
  txMsg(requestMsg, 14, DWT_START_TX_IMMEDIATE);
  for (int i = id; i < N; i++) {
    nodeRxStore();
  }
}

/* Protocol functions */

/**
 * @brief Handler function for when the initiating timer timeouts.
 * 
 * @param pContext General parameter that can be passed into the handler.
 */
static void initTimerHandler(void *pContext)
{
  // Stop receiving frames
  dwt_forcetrxoff();
  isInitiating = false;
}

/**
 * @brief Handler function for when the sleeping timer timeouts.
 * 
 * @param pContext General parameter that can be passed into the handler.
 */
static void sleepTimerHandler(void *pContext)
{
  isSleeping = false;
  dwWake();
}

/**
 * @brief Initialises the cycle timings.
 * 
 * @details These timings are the cycle, active and sleep periods.
 */
static void initCycleTimings(void)
{
  cyclePeriod = 1000000 / RANGE_FREQ; // Convert from seconds to microseconds.
  activePeriod = ((2 * N - 1) * (TX_INTERVAL)); // Number of intervals in N nodes.
  sleepPeriod = cyclePeriod - activePeriod;
}

/**
 * @brief Listens for activity in the network at initial phase.
 * 
 */
static void initListen(void)
{
  isInitiating = true;

  // Start listening for one cycle
  dwt_rxenable(DWT_START_RX_IMMEDIATE);
  lowTimerStart(initTimer, cyclePeriod);

  // TODO: Determine how long to sleep after this listening. So the node wakes up at the correct time where protocol cycle
  // in the network begins.
}

/**
 * @brief Sets the TX delay for the first of the two messages.
 * 
 * @param nodeId ID of this node.
 */
static void setTxDelay(int nodeId)
{
  if (nodeId == 1)
  {
    return;
  }
  else
  {
    uint64 sysTime = (dwt_read32bitoffsetreg(SYS_TIME_ID, SYS_TIME_OFFSET)) << 8;
    uint64 intv = ((TX_INTERVAL * (nodeId - 1)) * UUS_TO_DWT_TIME);
    uint32 startTime = (sysTime + intv) >> 8;
    dwt_setdelayedtrxtime(startTime);
  } 
}

/**
 * @brief Sets the TX delay for the second of the two messages.
 * 
 * @param nodeId ID of this node.
 */
static void setFnDelay(int nodeId)
{
  uint64 sysTime = (dwt_read32bitoffsetreg(SYS_TIME_ID, SYS_TIME_OFFSET)) << 8;
  uint64 intv = ((TX_INTERVAL * N) * UUS_TO_DWT_TIME);
  uint32 startTime = (sysTime + intv) >> 8;
  dwt_setdelayedtrxtime(startTime);
}