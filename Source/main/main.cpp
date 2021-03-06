/***************************************************************************************************
 * Copyright 2019 ContextQuickie
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **************************************************************************************************/

/***************************************************************************************************
 * INCLUDES
 **************************************************************************************************/
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "nvs_flash.h"

extern "C" {
#include "Axp192.h"
#include "Neo6.h"
#include "Display.h"
}

#include "TheThingsNetwork.h"
#include "TheThingsNetwork_Cfg.h"
/***************************************************************************************************
 * DEFINES
 **************************************************************************************************/
#define SLEEP_TIME_FROM_SECONDS(seconds)        (seconds * 1000 * 1000)
#define SLEEP_TIME_FROM_MINUTES(minutes)        (SLEEP_TIME_FROM_SECONDS(minutes * 60))
/***************************************************************************************************
 * DECLARATIONS
 **************************************************************************************************/
static void InitializeMemory();
static void InitializeComponents();
static void Task1000ms(void *pvParameters);
static UBaseType_t TaskStackMonitoring(UBaseType_t lastRemainingStack);

/***************************************************************************************************
 * CONSTANTS
 **************************************************************************************************/

/***************************************************************************************************
 * VARIABLES
 **************************************************************************************************/
static TheThingsNetwork ttn;
/***************************************************************************************************
 * IMPLEMENTATION
 **************************************************************************************************/
extern "C" void app_main()
{
  /* Print chip information */
  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);
  printf("This is ESP32 chip with %d CPU cores, WiFi%s%s, ", chip_info.cores,
         (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
         (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");

  printf("silicon revision %d, ", chip_info.revision);

  printf("%dMB %s flash\n", spi_flash_get_chip_size() / (1024 * 1024),
         (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

  fflush(stdout);

  InitializeMemory();

  InitializeComponents();

  Axp192_SetPwronWakeupFunctionState(Axp192_On);

  /* Required for Axp192_GetBatteryDischargeCurrent */
  Axp192_SetAdcState(Axp192_BatteryCurrentAdc, Axp192_On);

  /* Reuired for Axp192_GetBatteryCharge */
  Axp192_SetCoulombSwitchControlState(Axp192_On);

  if (xTaskCreatePinnedToCore(Task1000ms, "Task1000ms", 4096, NULL, 10, NULL, 0) == pdPASS)
  {
    /* The task was created.  Use the task's handle to delete the task. */
    vTaskDelete(NULL);
  }
  else
  {
    for (int i = 10; i >= 0; i--)
    {
      printf("Restarting in %d seconds...\n", i);
      vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    printf("Restarting now.\n");
    fflush(stdout);
    esp_restart();
  }
}

static void InitializeMemory()
{
  Axp192_InitMemory();
  Neo6_InitMemory();
  Display_InitMemory();
}

static void InitializeComponents()
{
  Axp192_Init();

  /* Enable voltage on LDO3 for NEO6 GPS module */
  Axp192_SetLdo3Voltage(3300);
  Axp192_SetLdo3State(Axp192_On);
  Neo6_Init();

  /* Enable voltage on DCDC1 for display */
  Axp192_SetDcDc1Voltage(2500);
  Axp192_SetDcDc1State(Axp192_On);
  Display_Init();

  /* Enable voltage on LDO2 for SX1276 LORA module */
  Axp192_SetLdo2State(Axp192_On);

  /* NVS is required for storing LoRa data */
  ESP_ERROR_CHECK(nvs_flash_init());

  // Initialize the GPIO ISR handler service
  ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));

  // Initialize SPI bus
  spi_bus_config_t spi_bus_config;
  spi_bus_config.miso_io_num = TTN_PIN_SPI_MISO;
  spi_bus_config.mosi_io_num = TTN_PIN_SPI_MOSI;
  spi_bus_config.sclk_io_num = TTN_PIN_SPI_SCLK;
  spi_bus_config.quadwp_io_num = -1;
  spi_bus_config.quadhd_io_num = -1;
  spi_bus_config.max_transfer_sz = 0;
  ESP_ERROR_CHECK(spi_bus_initialize(TTN_SPI_HOST, &spi_bus_config, TTN_SPI_DMA_CHAN));

  // Configure the SX127x pins
  ttn.configurePins(TTN_SPI_HOST, TTN_PIN_NSS, TTN_PIN_RXTX, TTN_PIN_RST, TTN_PIN_DIO0, TTN_PIN_DIO1);

  // The below line can be commented after the first run as the data is saved in NVS
  ttn.provision(TTN_DEVICE_EUI, TTN_APPLICATION_EUI, TTN_APPLICATION_SESSION_KEY);

  ttn.join();
}

static void Task1000ms(void *pvParameters)
{
  TickType_t xLastWakeTime = xTaskGetTickCount();
  UBaseType_t remainingTaskStack = INT32_MAX;
  uint16_t lastBatteryVoltage = UINT16_MAX;
  uint16_t lastChargeCurrent = UINT16_MAX;
  uint16_t lastDischargeCurrent = UINT16_MAX;
  uint32_t lastBatteryCharge = UINT32_MAX;
  uint16_t Task1000ms_SecondCounter = 0;
  char stringBuffer[20];
  for (;;)
  {
    // Wait for the next cycle.
    vTaskDelayUntil(&xLastWakeTime, 1000 / portTICK_PERIOD_MS);
    Task1000ms_SecondCounter++;
    remainingTaskStack = TaskStackMonitoring(remainingTaskStack);

    uint16_t currentBatteryVoltage = Axp192_GetBatteryVoltage();
    uint16_t currentChargeCurrent = Axp192_GetBatteryChargeCurrent();
    uint16_t currentDisChargeCurrent = Axp192_GetBatteryDischargeCurrent();
    uint32_t currentBatteryCharge = Axp192_GetBatteryCharge();
    if ((currentBatteryVoltage != lastBatteryVoltage) ||
        (currentChargeCurrent != lastChargeCurrent) ||
        (currentDisChargeCurrent != lastDischargeCurrent) ||
        (currentBatteryCharge != lastBatteryCharge))
    {
      Display_Clear();
      snprintf(stringBuffer, sizeof(stringBuffer), "Ubat: %4d mV", currentBatteryVoltage);
      Display_DrawString(0, 15, stringBuffer);
      snprintf(stringBuffer, sizeof(stringBuffer), "Icharge: %4d mA", currentChargeCurrent);
      Display_DrawString(0, 30, stringBuffer);
      snprintf(stringBuffer, sizeof(stringBuffer), "Ibat: %4d mA", currentDisChargeCurrent);
      Display_DrawString(0, 45, stringBuffer);
      snprintf(stringBuffer, sizeof(stringBuffer), "Cbat: %5d mAh", currentBatteryCharge);
      Display_DrawString(0, 60, stringBuffer);
      Display_SendBuffer();
    }

    lastBatteryVoltage = currentBatteryVoltage;
    lastChargeCurrent = currentChargeCurrent;
    lastDischargeCurrent = currentDisChargeCurrent;
    lastBatteryCharge = currentBatteryCharge;

    if ((Task1000ms_SecondCounter % 100) == 0)
    {
      Neo6_GeodeticPositionSolutionType geodeticPositionSolution;
      if (Neo6_GetGeodeticPositionSolution(&geodeticPositionSolution) == Neo6_Success)
      {
        ESP_LOGI(__FUNCTION__, "Sending TTN data");
        ttn.transmitMessage((uint8_t*)&geodeticPositionSolution, sizeof(Neo6_GeodeticPositionSolutionType), 1, false);
      }
    }

    if ((Task1000ms_SecondCounter % 150) == 0)
    {
      ESP_LOGI(__FUNCTION__, "Shutdown");

      /* Turn off display */
      Display_DeInit();

      /* TODO: Call to Axp192_SetDcDc1State will cause I2C communication errors during wakeup */
      /* Axp192_SetDcDc1State(Axp192_Off); */

      /* Turn off LORA */
      Axp192_SetLdo2State(Axp192_Off);

      /* Turn off GPS */
      Axp192_SetLdo3State(Axp192_Off);

      Axp192_DeInit();
      esp_deep_sleep(SLEEP_TIME_FROM_MINUTES(60llu));
    }
  }
}

static UBaseType_t TaskStackMonitoring(UBaseType_t lastRemainingStack)
{
  UBaseType_t currentRemainingStack = uxTaskGetStackHighWaterMark(NULL);
  if (currentRemainingStack != lastRemainingStack)
  {
    printf("Free stack in %s: %d Bytes\n", pcTaskGetTaskName(NULL), currentRemainingStack);
  }

  return currentRemainingStack;
}
