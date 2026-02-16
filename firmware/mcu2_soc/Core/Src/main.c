/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body (MCU2 SoC MVP-0)
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "gpio.h"
#include "usart.h"

/* USER CODE BEGIN Includes */
#ifdef __cplusplus
#include "ncomm_mcu2.hpp"
#endif
/* USER CODE END Includes */

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN PV */
#ifdef __cplusplus
static ncomm::NcommMcu2 g_mcu2;
#endif
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);

/* USER CODE BEGIN PFP */
/* USER CODE END PFP */

/* USER CODE BEGIN 0 */

#ifdef __cplusplus
extern "C" void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  // USART3 = link MCU2<->MCU1 (1M)
  if (huart == &huart3)
  {
    // Требует accessor в NcommMcu2:
    //   uint8_t last_rx_byte() const { return rx_byte_; }
    g_mcu2.on_rx_byte(g_mcu2.last_rx_byte());
  }
  else if (huart == &huart4)
  {
    // UART4 = link MCU2<->MCU3 (500k)
    // Пока MVP-0: можно позже добавить отдельный парсер для UI команд
    // или проброс логов. Сейчас просто игнорируем.
    // (Если решишь принимать UI-кадры здесь — делаем второй инстанс протокола.)
  }
}
#endif

/* USER CODE END 0 */

int main(void)
{
  /* MCU Configuration--------------------------------------------------------*/

  HAL_Init();
  SystemClock_Config();

  MX_GPIO_Init();
  MX_USART3_UART_Init();   // MCU1<->MCU2 @1_000_000
  MX_UART4_Init();         // MCU2<->MCU3 @500_000

  /* USER CODE BEGIN 2 */
#ifdef __cplusplus
  // Инициализация логики MCU2 (парсер протокола, команды, VAD статус и т.д.)
  g_mcu2.init(&huart3, &huart4);
#endif
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
#ifdef __cplusplus
    g_mcu2.tick();
#endif
    // Небольшая пауза, чтобы не крутиться на 100% CPU в MVP
    HAL_Delay(1);
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @note  MVP-0: оставляем дефолт / или позже генерим из CubeMX.
  */
void SystemClock_Config(void)
{
  // MVP-0: можно оставить пустым как сейчас (как у тебя в MCU1),
  // но для стабильного тайминга UART/I2S лучше потом сгенерить CubeMX clock tree.
}
