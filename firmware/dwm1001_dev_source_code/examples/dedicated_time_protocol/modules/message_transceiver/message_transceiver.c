#include "message_transceiver.h"

/* Local function prototypes. */

/*! ------------------------------------------------------------------------------------------------------------------
 * @fn txMsg()
 *
 * @brief transmit a frame.
 *
 * @param msg - pointer to the message (array) to be transmitted.
 *        mode - transmission mode to be used in transmission.
 *
 * @return the status of transmission.
 */
TxStatus txMsg(uint8 *msg, int msgLen, uint8 mode) {
  int ret;

  /* Write frame data to DW1000 and prepare transmission. */
  dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_TXFRS);
  dwt_writetxdata(msgLen, msg, 0); /* Zero offset in TX buffer. */
  dwt_writetxfctrl(msgLen, 0, 1);

  /* Start transmission, indicating that a response is expected so that reception is enabled automatically after the frame is sent. */
  ret = dwt_starttx(mode);
  if (ret == DWT_SUCCESS) {
    /* Ensure transmission occurs. */
    return TX_SUCCESS;
  } else {
    return TX_ERROR;
  }
}

/*! ------------------------------------------------------------------------------------------------------------------
 * @fn rxMsg()
 *
 * @brief read frame data from device
 *
 * @param buffer - pointer to buffer that stores the received frame.
 * 
 * @return the status of reception.
 */
RxStatus rxMsg(uint8 *buffer) {
  uint32 frameLen;

  /* A frame has been received, read it into the local buffer. */
  frameLen = dwt_read32bitreg(RX_FINFO_ID) & RX_FINFO_RXFL_MASK_1023;
  if (frameLen <= RX_BUFFER_LEN) {
    dwt_readrxdata(buffer, frameLen, 0);
  } else {
    return RX_ERROR;
  }

  return RX_SUCCESS;
}

/**
 * @brief Writes the timestamps to be received by other nodes for the second TX.
 * 
 * @param msg msg the message_template struct that will be transmitted out.
 * @param tsTable 2D array containing the timestamps for every other nodes.
 */
void writeTx2(msg_template *msg, uint64 tsTable[NUM_STAMPS_PER_CYCLE][NODES]) {
  uint64 ts[NUM_STAMPS_PER_CYCLE / 2];
  int i;
  
  for (i = 0; i < NODES; i++)
  {
    if (i == NODE_ID)
    {
      continue;
    }

    // Retrieve values for each node and copy into data member at predefined slots.
    getHalfTs(tsTable, ts, NODE_ID, i);
    memcpy(msg->data + (i * NUM_STAMPS_PER_NODE * TS_LEN), &ts[IDX_TS_1], TS_LEN);
    memcpy(msg->data + (i * NUM_STAMPS_PER_NODE * TS_LEN) + TS_LEN, &ts[IDX_TS_2], TS_LEN);
    memcpy(msg->data + (i * NUM_STAMPS_PER_NODE * TS_LEN) + (TS_LEN * 2), &ts[IDX_TS_3], TS_LEN);
  }
}

/**
 * @brief Configure the register parameters for a transmission.
 * 
 * @param tsTable 2D array containing the timestamps for every other nodes.
 * @param txDelay amount of time (in UWB time) to delay the TX for.
 * @param refTs timestamp to begin the delay from.
 * @param txMsg2 pointer to the message to be transmitted.
 * 
 * @return true if delayed TX will be sent successfully.
 * @return false if delayed TX will fail.
 */
bool configTx(
  uint64 tsTable[NUM_STAMPS_PER_CYCLE][NODES],
  uint64 txDelay,
  uint64 refTs,
  msg_template *txMsg
)
{
  uint8 mode = DWT_START_TX_DELAYED | DWT_RESPONSE_EXPECTED;
  uint32 delay = (refTs + txDelay) >> 8;

  dwt_setdelayedtrxtime(delay);

  if (convertTx(txMsg, mode) == TX_SUCCESS)
  {
    return true;
  }
  else
  {
    return false;
  }
}

/**
 * @brief Converts and transmits the given message.
 * 
 * @param msg pointer to the message to be transmitted.
 * @param mode - if 0 immediate TX (no response expected) => DWT_START_TX_IMMEDIATE
 *               if 1 delayed TX (no response expected) => DWT_START_TX_DELAYED
 *               if 2 immediate TX (response expected - so the receiver will be automatically turned on after TX is done) => DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED
 *               if 3 delayed TX (response expected - so the receiver will be automatically turned on after TX is done) => DWT_START_TX_DELAYED | DWT_RESPONSE_EXPECTED
 */
TxStatus convertTx(msg_template *msg, uint8 mode)
{
  uint8 buf[MSG_LEN];

  dwt_forcetrxoff();
  convertToArr(*msg, buf);
  
  return txMsg(buf, MSG_LEN, mode);
}