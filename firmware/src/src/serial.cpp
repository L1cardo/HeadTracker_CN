/*
 * This file is part of the Head Tracker distribution (https://github.com/dlktdr/headtracker)
 * Copyright (c) 2021 Cliff Blackburn
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "serial.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/class/usb_cdc.h>
#include <zephyr/sys/reboot.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "io.h"

#include "htmain.h"
#include "soc_flash.h"
#include "trackersettings.h"
#include "ucrc16lib.h"
#include "boards/features.h"

LOG_MODULE_REGISTER(serial);

void serialrx_Process();
char *getJSONBuffer();
void parseData(JsonDocument &json);
uint16_t escapeCRC(uint16_t crc);
int buffersFilled();

// Connection state
uint32_t dtr = 0;

// Ring Buffers
uint8_t ring_buffer_tx[TX_RNGBUF_SIZE];  // transmit buffer
uint8_t ring_buffer_rx[RX_RNGBUF_SIZE];  // receive buffer
struct ring_buf ringbuf_tx;
struct ring_buf ringbuf_rx;
K_MUTEX_DEFINE(ring_tx_mutex);
K_MUTEX_DEFINE(ring_rx_mutex);

// JSON Data
char jsonbuffer[JSON_BUF_SIZE];
char *jsonbufptr = jsonbuffer;
JsonDocument json;

// Mutex to protect Sense & Data Writes
K_MUTEX_DEFINE(data_mutex);

// Flag that serial has been initalized
static struct k_poll_signal serialThreadRunSignal =
    K_POLL_SIGNAL_INITIALIZER(serialThreadRunSignal);
struct k_poll_event serialRunEvents[1] = {
    K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_SIGNAL, K_POLL_MODE_NOTIFY_ONLY, &serialThreadRunSignal),
};

const struct device *dev;

static void interrupt_handler(const struct device *dev, void *user_data)
{
  ARG_UNUSED(user_data);

  while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
    if (uart_irq_rx_ready(dev)) {
      int recv_len, rb_len;
      uint8_t buffer[64];

      // TODO: This is a blocking call. should it be in an interrupt, why?
      k_mutex_lock(&ring_rx_mutex, K_FOREVER);
      size_t len = MIN(ring_buf_space_get(&ringbuf_rx), sizeof(buffer));
      recv_len = uart_fifo_read(dev, buffer, len);
      rb_len = ring_buf_put(&ringbuf_rx, buffer, recv_len);
      k_mutex_unlock(&ring_rx_mutex);

      if (rb_len < recv_len) {
        // LOG_ERR("RX Ring Buffer Full");
      }
    }
  }
}

int serial_init()
{
  // Use the device tree alias to specify the UART device to use
  dev = DEVICE_DT_GET(DT_ALIAS(guiuart));
  if (!device_is_ready(dev)) {
		LOG_ERR("GUI UART device is not ready");
		return -1;
  }

  ring_buf_init(&ringbuf_tx, sizeof(ring_buffer_tx), ring_buffer_tx);
  k_mutex_init(&ring_tx_mutex);

  ring_buf_init(&ringbuf_rx, sizeof(ring_buffer_rx), ring_buffer_rx);
  k_mutex_init(&ring_rx_mutex);

  /* They are optional, we use them to test the interrupt endpoint */
#if defined(DT_N_INST_0_zephyr_cdc_acm_uart)
  int ret = uart_line_ctrl_set(dev, UART_LINE_CTRL_DCD, 1);
  ret = uart_line_ctrl_set(dev, UART_LINE_CTRL_DSR, 1);
#endif

  /* Wait 1 sec for the host to do all settings */
  k_busy_wait(1000000);

  uart_irq_callback_set(dev, interrupt_handler);

  /* Enable rx interrupts */
  uart_irq_rx_enable(dev);

  // Start Serial Thread
  k_poll_signal_raise(&serialThreadRunSignal, 1);
  LOG_INF("Serial Thread Signal Raised");

  return 0;
}

void serial_Thread()
{
  uint8_t buffer[256];
  static uint32_t datacounter = 0;
  LOG_INF("Serial Thread Loaded");

  while (1) {
    k_poll(serialRunEvents, 1, K_FOREVER);
    k_msleep(SERIAL_PERIOD);

    if (k_sem_count_get(&flashWriteSemaphore) == 1) {
      continue;
    }

    // If serial not open, abort all transfers, clear buffer
    uint32_t new_dtr = 0;
    uart_line_ctrl_get(dev, UART_LINE_CTRL_DTR, &new_dtr);

    k_mutex_lock(&ring_tx_mutex, K_FOREVER);

#if defined(DT_N_INST_0_zephyr_cdc_acm_uart)
    // lost connection
    if (dtr && !new_dtr) {
      trkset.stopAllData();
    }

    // gaining new connection
    if (!dtr && new_dtr) {

      // Force bootloader if baud set to 1200bps TODO (Test Me)
      /*uint32_t baud=0;
      uart_line_ctrl_get(dev,UART_LINE_CTRL_BAUD_RATE, &baud);
      if(baud == 1200) {
        (*((volatile uint32_t *) 0x20007FFCul)) = 0x07738135;
        NVIC_SystemReset();
      }*/
    }
    // Port is open, send data
    if (new_dtr) {
#endif
      int rb_len = ring_buf_get(&ringbuf_tx, buffer, sizeof(buffer));
      if (rb_len) {
        int send_len = uart_fifo_fill(dev, buffer, rb_len); // TODO this is wrong, needs to be in an ISR according to the Zephyr docs
        if (send_len < rb_len) {
          // LOG_ERR("USB CDC Ring Buffer Full, Dropped data");
        }
      }

#if defined(DT_N_INST_0_zephyr_cdc_acm_uart)
    }
    dtr = new_dtr;
#endif
    k_mutex_unlock(&ring_tx_mutex);
    serialrx_Process();

    // Data output
    if (datacounter++ >= DATA_PERIOD) {
      datacounter = 0;

      // If sense thread is writing, wait until complete
      k_mutex_lock(&data_mutex, K_FOREVER);
      json.clear();
      trkset.setJSONData(json);
      if (json.size()) {
        json["Cmd"] = "Data";
        serialWriteJSON(json);
      }
      k_mutex_unlock(&data_mutex);
    }
  }
}

void serialrx_Process()
{
  char sc = 0;

  // Get byte by byte data from the serial receive ring buffer
  k_mutex_lock(&ring_rx_mutex, K_FOREVER);
  while (ring_buf_get(&ringbuf_rx, (uint8_t *)&sc, 1)) {
    if (sc == 0x02) {           // Start Of Text Character, clear buffer
      jsonbufptr = jsonbuffer;  // Reset Buffer

    } else if (sc == 0x03) {  // End of Text Characher, parse JSON data
      *jsonbufptr = 0;        // Null terminate
      JSON_Process(jsonbuffer);
      jsonbufptr = jsonbuffer;  // Reset Buffer
    } else {
      // Check how much free data is in the buffer
      if (jsonbufptr >= jsonbuffer + sizeof(jsonbuffer) - 3) {
        LOG_ERR("Error JSON data too long, overflow");
        jsonbufptr = jsonbuffer;  // Reset Buffer

        // Add data to buffer
      } else {
        *(jsonbufptr++) = sc;
      }
    }
  }
  k_mutex_unlock(&ring_rx_mutex);
}

void JSON_Process(char *jsonbuf)
{
  // CRC Check Data
  int len = strlen(jsonbuf);
  if (len > 2) {
    k_mutex_lock(&ring_tx_mutex, K_FOREVER);
    uint16_t calccrc = escapeCRC(uCRC16Lib::calculate(jsonbuf, len - sizeof(uint16_t)));
    if (calccrc != *(uint16_t *)(jsonbuf + len - sizeof(uint16_t))) {
      serialWrite("\x15\r\n");  // Not-Acknowledged
      k_mutex_unlock(&ring_tx_mutex);
      return;
    } else {
      serialWrite("\x06\r\n");  // Acknowledged
    }
    // Remove CRC from end of buffer
    jsonbuf[len - sizeof(uint16_t)] = 0;

    k_mutex_lock(&data_mutex, K_FOREVER);
    DeserializationError de = deserializeJson(json, jsonbuf);
    if (de) {
      if (de == DeserializationError::IncompleteInput)
        LOG_ERR("DeserializeJson() Failed - Incomplete Input");
      else if (de == DeserializationError::InvalidInput)
        LOG_ERR("DeserializeJson() Failed - Invalid Input");
      else if (de == DeserializationError::NoMemory)
        LOG_ERR("DeserializeJson() Failed - NoMemory");
      else if (de == DeserializationError::EmptyInput)
        LOG_ERR("DeserializeJson() Failed - Empty Input");
      else if (de == DeserializationError::TooDeep)
        LOG_ERR("DeserializeJson() Failed - TooDeep");
      else
        LOG_ERR("DeserializeJson() Failed - Other");
    } else {
      // Parse The JSON Data in dataparser.cpp
      parseData(json);
    }
    k_mutex_unlock(&data_mutex);
    k_mutex_unlock(&ring_tx_mutex);
  }
}

// New JSON data received from the PC
void parseData(JsonDocument &json)
{
  JsonVariant v = json["Cmd"];
  if (v.isNull()) {
    LOG_ERR("Invalid JSON, No Command");
    return;
  }

  // For strcmp;
  const char *command = v;

  // Reset Center
  if (strcmp(command, "RstCnt") == 0) {
    // TODO we should also log when the button on the device issues a Reset Center
    LOG_INF("Resetting Center");
    pressButton();

    // Settings Sent from UI
  } else if (strcmp(command, "Set") == 0) {
    trkset.loadJSONSettings(json);
    LOG_INF("Storing Settings");

    // Save to Flash
  } else if (strcmp(command, "Flash") == 0) {
    LOG_INF("Saving to Flash");
    k_sem_give(&saveToFlash_sem);

    // Erase
  } else if (strcmp(command, "Erase") == 0) {
    LOG_INF("Clearing Flash");
    socClearFlash();

    // Reboot
  } else if (strcmp(command, "Reboot") == 0) {
    sys_reboot(SYS_REBOOT_COLD);

    // Force Bootloader
  } else if (strcmp(command, "Boot") == 0) {
#if defined(ARDUINO_BOOTLOADER)
    (*((volatile uint32_t *)0x20007FFCul)) = 0x07738135;
#elif defined(SEEED_BOOTLOADER)
    __disable_irq();
#define DFU_MAGIC_UF2_RESET           0x57
    NRF_POWER->GPREGRET = DFU_MAGIC_UF2_RESET;
#endif
    sys_reboot(SYS_REBOOT_COLD);

    // Get settings
  } else if (strcmp(command, "Get") == 0) {
    LOG_INF("Sending Settings");
    json.clear();
    trkset.setJSONSettings(json);
    json["Cmd"] = "Set";
    serialWriteJSON(json);

    // Im Here Received, Means the GUI is running
  } else if (strcmp(command, "IH") == 0) {


    // Get a List of All Data Items
  } else if (strcmp(command, "DatLst") == 0) {
    json.clear();
    trkset.setJSONDataList(json);
    json["Cmd"] = "DataList";
    serialWriteJSON(json);

    // Stop All Data Items
  } else if (strcmp(command, "D--") == 0) {
    LOG_INF("Clearing Data List");
    trkset.stopAllData();

    // Request Data Items
  } else if (strcmp(command, "RD") == 0) {
    LOG_INF("Data Added/Remove");
    // using C++11 syntax (preferred):
    JsonObject root = json.as<JsonObject>();
    for (JsonPair kv : root) {
      if (kv.key() == "Cmd") continue;
      trkset.setDataItemSend(kv.key().c_str(), kv.value().as<bool>());
    }

    // Firmware Reqest
  } else if (strcmp(command, "FW") == 0) {
    JsonDocument fwjson;
    fwjson["Cmd"] = "FW";
    fwjson["Vers"] = STRINGIFY(FW_VER_TAG);
    fwjson["Hard"] = FW_BOARD;
    fwjson["Git"] = STRINGIFY(FW_GIT_REV);
    serialWriteJSON(fwjson);

    // Board Features Request
  } else if (strcmp(command, "FE") == 0) {
    json.clear();
    getBoardFeatures(json);
    json["Cmd"] = "FE";
    serialWriteJSON(json);

    // Unknown Command
  } else {
    LOG_WRN("Unknown Command");
    return;
  }
}

// Remove any of the escape characters
uint16_t escapeCRC(uint16_t crc)
{
  // Characters to escape out
  uint8_t crclow = crc & 0xFF;
  uint8_t crchigh = (crc >> 8) & 0xFF;
  if (crclow == 0x00 || crclow == 0x02 || crclow == 0x03 || crclow == 0x06 || crclow == 0x15)
    crclow ^= 0xFF;  //?? why not..
  if (crchigh == 0x00 || crchigh == 0x02 || crchigh == 0x03 || crchigh == 0x06 || crchigh == 0x15)
    crchigh ^= 0xFF;  //?? why not..
  return (uint16_t)crclow | ((uint16_t)crchigh << 8);
}

// Base write function.

void serialWrite(const char *data, int len)
{
  k_mutex_lock(&ring_tx_mutex, K_FOREVER);
  if (ring_buf_space_get(&ringbuf_tx) < (uint32_t)len) {  // Not enough room, drop it.
    k_mutex_unlock(&ring_tx_mutex);
    return;
  }
  int rb_len = ring_buf_put(&ringbuf_tx, (uint8_t *)data, len);
  k_mutex_unlock(&ring_tx_mutex);
  if (rb_len != len) {
    // TODO: deal with this case

  }
}

void serialWrite(const char *data)
{
  // Append Output to the serial output buffer
  serialWrite(data, strlen(data));
}

int serialWriteF(const char *format, ...)
{
  va_list vArg;
  va_start(vArg, format);
  char buf[256];
  int len = vsnprintf(buf, sizeof(buf), format, vArg);
  va_end(vArg);
  serialWrite(buf, len);
  return len;
}

// FIX Me to Not use as Much Stack.
void serialWriteJSON(JsonDocument &json)
{
  char data[TX_RNGBUF_SIZE];

  k_mutex_lock(&ring_tx_mutex, K_FOREVER);
  int br = serializeJson(json, data + 1, TX_RNGBUF_SIZE - 7);
  uint16_t calccrc = escapeCRC(uCRC16Lib::calculate(data, br));

  if (br + 7 > TX_RNGBUF_SIZE) {
    k_mutex_unlock(&ring_tx_mutex);
    return;
  }

  data[0] = 0x02;
  data[br + 1] = (calccrc >> 8) & 0xFF;
  data[br + 2] = calccrc & 0xFF;
  data[br + 3] = 0x03;
  data[br + 4] = '\r';
  data[br + 5] = '\n';

  serialWrite(data, br + 6);
  k_mutex_unlock(&ring_tx_mutex);
}