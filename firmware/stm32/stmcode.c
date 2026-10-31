/* ===========================================================================
 * Smart Parking System - STM32F401CCU6 (Keil, STM32Cube HAL "Classic")
 * + ESP32 Wi-Fi slot booking over UART (USART2: PA2 = TX, PA3 = RX, 115200 8N1)
 * + RC522 RFID card authentication over SPI1
 *
 * Flow:
 *   User may pre-book a slot on the web page (ESP32).
 *   Car at entrance IR (PB10)
 *     free slot that is NOT booked -> short beep, OLED "GO TO SLOT n",
 *                                     gate opens automatically
 *     free slot but BOOKED         -> OLED "TAP YOUR CARD", RC522 reads the card,
 *                                     authorized UID -> "AUTHENTICATION DONE",
 *                                     "GO TO SLOT n", gate opens
 *                                     wrong card -> "WRONG CARD", try again
 *                                     no card in AUTH_TIMEOUT_MS -> gate stays closed
 *     no usable slot               -> 2 long beeps, OLED "NO SLOT", gate closed
 *   Slot IR sensors -> slot LEDs + OLED (FREE / RESERVED / OCCUPIED)
 *   light-sensor module (PB0, digital) -> both parking lights (ON when dark, OFF when bright)
 *
 * RC522 wiring (3.3 V ONLY):
 *   SDA(SS) -> PB5   SCK -> PA5   MOSI -> PA7   MISO -> PA6   RST -> PB1
 *
 * UART protocol (ASCII lines ending in \n):
 *   STM32 -> ESP32 : "S,o1,o2,o3"   slot occupancy (1 = occupied), on change + every 2 s
 *   ESP32 -> STM32 : "B,b1,b2,b3"   slot booked flags (1 = reserved)
 *
 * Runs on the default 16 MHz HSI clock (no clock setup needed).
 * Required RTE HAL components: Common, Cortex, DMA, GPIO, PWR, RCC, ADC, I2C, TIM, UART, SPI
 * =========================================================================== */
#include "stm32f4xx_hal.h"
#include <string.h>

/* ============================ USER SETTINGS ============================== */
#define IR_ACTIVE_LEVEL    GPIO_PIN_RESET  /* most IR modules output LOW when object detected */
#define LED_ON_WHEN_FREE   1               /* 1: LED on = slot available, 0: LED on = occupied */
#define LDR_DARK_LEVEL     GPIO_PIN_SET    /* PB0 level when the light-sensor module says "dark" (use GPIO_PIN_RESET if inverted) */

#define SERVO_CLOSED_US    1000            /* 0 degrees  (tune: some servos need ~500)  */
#define SERVO_OPEN_US      1500            /* 90 degrees (tune: ~1500 for 500-2500 us servos) */

#define ACCEPT_ANY_CARD    1               /* 1 = any card authenticates (use for first test), 0 = only AUTH_UIDS */
#define AUTH_TIMEOUT_MS    15000           /* how long to wait for a card tap */
#define ASSIGN_HOLD_MS     60000           /* slot is reserved for an entering car this long */
#define GATE_OPEN_MS       5000            /* gate stays open this long     */
#define UART_BAUD          115200

/* Authorized card/tag UIDs (4 bytes). Read yours from last_uid in the Keil debugger. */
static const uint8_t AUTH_UIDS[][4] = {
    {0xDE, 0xAD, 0xBE, 0xEF},
    {0x12, 0x34, 0x56, 0x78},
};
#define AUTH_COUNT (sizeof(AUTH_UIDS) / sizeof(AUTH_UIDS[0]))
/* ========================================================================= */

/* ---- Pins ---- */
#define BUZZ_PORT   GPIOB
#define BUZZ_PIN    GPIO_PIN_12
#define ENT_PORT    GPIOB
#define ENT_PIN     GPIO_PIN_10
#define LIGHT_PORT  GPIOA
#define LIGHT1_PIN  GPIO_PIN_8
#define LIGHT2_PIN  GPIO_PIN_9

#define RC_CS_PORT  GPIOB
#define RC_CS_PIN   GPIO_PIN_5
#define RC_RST_PORT GPIOB
#define RC_RST_PIN  GPIO_PIN_1

static GPIO_TypeDef *const SLOT_PORT[3]     = {GPIOA, GPIOA, GPIOA};
static const uint16_t      SLOT_PIN[3]      = {GPIO_PIN_0, GPIO_PIN_1, GPIO_PIN_4};
static const uint16_t      SLOT_LED_PIN[3]  = {GPIO_PIN_13, GPIO_PIN_14, GPIO_PIN_15}; /* GPIOB */

TIM_HandleTypeDef  htim3;
I2C_HandleTypeDef  hi2c1;
UART_HandleTypeDef huart2;
SPI_HandleTypeDef  hspi1;

/* Variables you can watch in the Keil debugger */
volatile uint8_t  occupied[3];
volatile uint8_t  booked[3];       /* from ESP32 */
static uint32_t   assigned_at[3];  /* tick when a car was sent to this slot (0 = none) */
volatile uint8_t  last_uid[4];     /* last card UID read by the RC522 */
volatile uint8_t  rc_version;      /* RC522 VersionReg: normally 0x91 / 0x92 */

static uint8_t oled_ok = 0;
static uint8_t rc_ok = 0;

void SysTick_Handler(void) { HAL_IncTick(); }

/* ======================================================================== */
/*                         UART link to ESP32 (USART2)                      */
/* ======================================================================== */
static volatile uint8_t rx_buf[128];
static volatile uint8_t rx_head, rx_tail;

/* Own IRQ handler: stores bytes in a ring buffer (no HAL UART callbacks needed) */
void USART2_IRQHandler(void)
{
    uint32_t sr = USART2->SR;
    if (sr & (USART_SR_RXNE | USART_SR_ORE)) {
        uint8_t b = (uint8_t)USART2->DR;           /* reading SR then DR clears RXNE / ORE */
        uint8_t next = (uint8_t)((rx_head + 1) & 127);
        if (next != rx_tail) { rx_buf[rx_head] = b; rx_head = next; }
    }
}

static void uart_init(void)
{
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART2_CLK_ENABLE();

    g.Pin = GPIO_PIN_2 | GPIO_PIN_3;               /* PA2 = TX, PA3 = RX */
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Alternate = GPIO_AF7_USART2;
    HAL_GPIO_Init(GPIOA, &g);

    huart2.Instance = USART2;
    huart2.Init.BaudRate = UART_BAUD;
    huart2.Init.WordLength = UART_WORDLENGTH_8B;
    huart2.Init.StopBits = UART_STOPBITS_1;
    huart2.Init.Parity = UART_PARITY_NONE;
    huart2.Init.Mode = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&huart2);

    __HAL_UART_ENABLE_IT(&huart2, UART_IT_RXNE);
    HAL_NVIC_SetPriority(USART2_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(USART2_IRQn);
}

static void uart_send(const char *s)
{
    HAL_UART_Transmit(&huart2, (uint8_t *)s, (uint16_t)strlen(s), 50);
}

static void uart_parse_line(const char *l)
{
    /* "B,x,y,z" : booked flags */
    if (l[0] == 'B' && l[1] == ',' && l[2] && l[3] == ',' && l[4] && l[5] == ',' && l[6]) {
        booked[0] = (l[2] == '1');
        booked[1] = (l[4] == '1');
        booked[2] = (l[6] == '1');
    }
}

static void uart_poll(void)
{
    static char line[24];
    static uint8_t n = 0;
    while (rx_tail != rx_head) {
        char c = (char)rx_buf[rx_tail];
        rx_tail = (uint8_t)((rx_tail + 1) & 127);
        if (c == '\n') {
            line[n] = 0;
            if (n) uart_parse_line(line);
            n = 0;
        } else if (c != '\r') {
            if (n < sizeof(line) - 1) line[n++] = c;
            else n = 0;                            /* overlong line: discard */
        }
    }
}

/* ======================================================================== */
/*                               BUZZER                                     */
/* ======================================================================== */
static void buzzer_beep(uint32_t ms)
{
    HAL_GPIO_WritePin(BUZZ_PORT, BUZZ_PIN, GPIO_PIN_SET);
    HAL_Delay(ms);
    HAL_GPIO_WritePin(BUZZ_PORT, BUZZ_PIN, GPIO_PIN_RESET);
}

static void buzzer_pattern(uint8_t count, uint32_t on_ms, uint32_t off_ms)
{
    for (uint8_t i = 0; i < count; i++) {
        buzzer_beep(on_ms);
        HAL_Delay(off_ms);
    }
}

/* ======================================================================== */
/*                         RC522 card reader (SPI1)                         */
/* ======================================================================== */
/* RC522 registers */
#define CommandReg    0x01
#define ComIrqReg     0x04
#define ErrorReg      0x06
#define FIFODataReg   0x09
#define FIFOLevelReg  0x0A
#define BitFramingReg 0x0D
#define ModeReg       0x11
#define TxControlReg  0x14
#define TxASKReg      0x15
#define TModeReg      0x2A
#define TPrescalerReg 0x2B
#define TReloadRegH   0x2C
#define TReloadRegL   0x2D
#define VersionReg    0x37

static void rc_write(uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { (uint8_t)((reg << 1) & 0x7E), val };
    HAL_GPIO_WritePin(RC_CS_PORT, RC_CS_PIN, GPIO_PIN_RESET);
    HAL_SPI_Transmit(&hspi1, tx, 2, 10);
    HAL_GPIO_WritePin(RC_CS_PORT, RC_CS_PIN, GPIO_PIN_SET);
}

static uint8_t rc_read(uint8_t reg)
{
    uint8_t tx[2] = { (uint8_t)(((reg << 1) & 0x7E) | 0x80), 0 };
    uint8_t rx[2] = {0, 0};
    HAL_GPIO_WritePin(RC_CS_PORT, RC_CS_PIN, GPIO_PIN_RESET);
    HAL_SPI_TransmitReceive(&hspi1, tx, rx, 2, 10);
    HAL_GPIO_WritePin(RC_CS_PORT, RC_CS_PIN, GPIO_PIN_SET);
    return rx[1];
}

static void rc_setbits(uint8_t reg, uint8_t m) { rc_write(reg, (uint8_t)(rc_read(reg) | m)); }
static void rc_clrbits(uint8_t reg, uint8_t m) { rc_write(reg, (uint8_t)(rc_read(reg) & (uint8_t)~m)); }

/* Sends tx[] to the card, returns 1 and fills rx[] if a valid answer came back */
static uint8_t rc_transceive(const uint8_t *tx, uint8_t txLen, uint8_t bits,
                             uint8_t *rx, uint8_t *rxLen)
{
    rc_write(ComIrqReg, 0x7F);            /* clear all IRQ flags */
    rc_write(FIFOLevelReg, 0x80);         /* flush FIFO */
    rc_write(CommandReg, 0x00);           /* idle */
    for (uint8_t i = 0; i < txLen; i++) rc_write(FIFODataReg, tx[i]);
    rc_write(BitFramingReg, bits);
    rc_write(CommandReg, 0x0C);           /* transceive */
    rc_setbits(BitFramingReg, 0x80);      /* start send */

    uint32_t t0 = HAL_GetTick();
    uint8_t irq;
    do { irq = rc_read(ComIrqReg); }
    while (!(irq & 0x31) && (HAL_GetTick() - t0) < 30);

    rc_clrbits(BitFramingReg, 0x80);
    rc_write(CommandReg, 0x00);                  /* back to idle */
    if (!(irq & 0x30)) return 0;                 /* timeout / no card */
    if (rc_read(ErrorReg) & 0x13) return 0;      /* buffer overflow, parity, protocol error */

    uint8_t n = rc_read(FIFOLevelReg);
    if (n == 0 || n > *rxLen) return 0;
    for (uint8_t i = 0; i < n; i++) rx[i] = rc_read(FIFODataReg);
    *rxLen = n;
    return 1;
}

/* Returns 1 and a 4-byte UID if a card is in the field */
static uint8_t rc_read_uid(uint8_t uid[4])
{
    uint8_t buf[8], len;

    /* REQA (7-bit frame) */
    uint8_t reqa = 0x26;
    len = sizeof(buf);
    if (!rc_transceive(&reqa, 1, 7, buf, &len) || len != 2) return 0;

    /* Anti-collision, cascade level 1 */
    uint8_t ac[2] = {0x93, 0x20};
    len = sizeof(buf);
    if (!rc_transceive(ac, 2, 0, buf, &len) || len != 5) return 0;

    if ((buf[0] ^ buf[1] ^ buf[2] ^ buf[3]) != buf[4]) return 0;   /* BCC check */
    for (uint8_t i = 0; i < 4; i++) uid[i] = buf[i];
    return 1;
}

/* Briefly switches the RF field off/on so the card resets and can be read again */
static void rc_field_reset(void)
{
    rc_clrbits(TxControlReg, 0x03);
    HAL_Delay(5);
    rc_setbits(TxControlReg, 0x03);
}

static void rc522_init(void)
{
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_SPI1_CLK_ENABLE();

    g.Pin = GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7;      /* SCK, MISO, MOSI */
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(GPIOA, &g);

    g.Pin = RC_CS_PIN | RC_RST_PIN;                    /* CS, RST */
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &g);
    HAL_GPIO_WritePin(RC_CS_PORT, RC_CS_PIN, GPIO_PIN_SET);

    hspi1.Instance = SPI1;
    hspi1.Init.Mode = SPI_MODE_MASTER;
    hspi1.Init.Direction = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
    hspi1.Init.NSS = SPI_NSS_SOFT;
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;   /* 16 MHz / 8 = 2 MHz */
    hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    HAL_SPI_Init(&hspi1);

    /* Hardware reset, then soft reset */
    HAL_GPIO_WritePin(RC_RST_PORT, RC_RST_PIN, GPIO_PIN_RESET);
    HAL_Delay(10);
    HAL_GPIO_WritePin(RC_RST_PORT, RC_RST_PIN, GPIO_PIN_SET);
    HAL_Delay(50);
    rc_write(CommandReg, 0x0F);
    HAL_Delay(50);

    rc_version = rc_read(VersionReg);                  /* 0x91 / 0x92 normally; clones may differ */
    rc_ok = (rc_version != 0x00 && rc_version != 0xFF);
    if (!rc_ok) return;

    rc_write(TModeReg, 0x8D);
    rc_write(TPrescalerReg, 0x3E);
    rc_write(TReloadRegH, 0);
    rc_write(TReloadRegL, 30);
    rc_write(TxASKReg, 0x40);
    rc_write(ModeReg, 0x3D);
    rc_setbits(TxControlReg, 0x03);                    /* antenna on */
}

static uint8_t uid_authorized(const uint8_t uid[4])
{
#if ACCEPT_ANY_CARD
    (void)uid;
    return 1;
#else
    for (uint32_t i = 0; i < AUTH_COUNT; i++)
        if (memcmp(uid, AUTH_UIDS[i], 4) == 0) return 1;
    return 0;
#endif
}

/* ======================================================================== */
/*                               SSD1306 OLED                               */
/* ======================================================================== */
#define OLED_ADDR (0x3C << 1)
static uint8_t oled_buf[1024];

static const uint8_t font_digits[10][5] = {
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},
    {0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},{0x36,0x49,0x49,0x49,0x36},
    {0x06,0x49,0x49,0x29,0x1E}
};
static const uint8_t font_alpha[26][5] = {
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43}
};

static const uint8_t *glyph(char c)
{
    static const uint8_t sp[5]    = {0x00,0x00,0x00,0x00,0x00};
    static const uint8_t excl[5]  = {0x00,0x00,0x5F,0x00,0x00};
    static const uint8_t dash[5]  = {0x08,0x08,0x08,0x08,0x08};
    static const uint8_t dot[5]   = {0x00,0x60,0x60,0x00,0x00};
    static const uint8_t colon[5] = {0x00,0x36,0x36,0x00,0x00};
    static const uint8_t slash[5] = {0x20,0x10,0x08,0x04,0x02};

    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return font_alpha[c - 'A'];
    if (c >= '0' && c <= '9') return font_digits[c - '0'];
    switch (c) {
        case '!': return excl;
        case '-': return dash;
        case '.': return dot;
        case ':': return colon;
        case '/': return slash;
        default:  return sp;
    }
}

static void oled_cmd(uint8_t c)
{
    HAL_I2C_Mem_Write(&hi2c1, OLED_ADDR, 0x00, I2C_MEMADD_SIZE_8BIT, &c, 1, 50);
}

static void oled_clear(void) { memset(oled_buf, 0, sizeof(oled_buf)); }

static void oled_str(uint8_t x, uint8_t row, const char *s)   /* row = 0..7 */
{
    while (*s && x <= 122 && row < 8) {
        const uint8_t *g = glyph(*s++);
        for (uint8_t i = 0; i < 5; i++) oled_buf[row * 128 + x + i] = g[i];
        oled_buf[row * 128 + x + 5] = 0;
        x += 6;
    }
}

static void oled_update(void)
{
    if (!oled_ok) return;
    oled_cmd(0x21); oled_cmd(0); oled_cmd(127);   /* column range */
    oled_cmd(0x22); oled_cmd(0); oled_cmd(7);     /* page range   */
    HAL_I2C_Mem_Write(&hi2c1, OLED_ADDR, 0x40, I2C_MEMADD_SIZE_8BIT, oled_buf, 1024, 200);
}

static void oled_init(void)
{
    static const uint8_t seq[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14,
        0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12, 0x81, 0xCF, 0xD9, 0xF1,
        0xDB, 0x40, 0xA4, 0xA6, 0xAF
    };
    oled_ok = (HAL_I2C_IsDeviceReady(&hi2c1, OLED_ADDR, 3, 50) == HAL_OK);
    if (!oled_ok) return;
    for (uint8_t i = 0; i < sizeof(seq); i++) oled_cmd(seq[i]);
    oled_clear();
    oled_update();
}

static void i2c_init(void)
{
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_I2C1_CLK_ENABLE();

    g.Pin = GPIO_PIN_8 | GPIO_PIN_9;            /* SCL, SDA */
    g.Mode = GPIO_MODE_AF_OD;
    g.Pull = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Alternate = GPIO_AF4_I2C1;
    HAL_GPIO_Init(GPIOB, &g);

    hi2c1.Instance = I2C1;
    hi2c1.Init.ClockSpeed = 400000;
    hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
    hi2c1.Init.OwnAddress1 = 0;
    hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    HAL_I2C_Init(&hi2c1);
}

/* ======================================================================== */
/*                               SERVO                                      */
/* ======================================================================== */
static int servo_cur_us = SERVO_CLOSED_US;

static void servo_write(int us)
{
    servo_cur_us = us;
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, (uint32_t)us);
}

static void servo_init(void)
{
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_TIM3_CLK_ENABLE();

    g.Pin = GPIO_PIN_4;                         /* PB4 = TIM3_CH1 */
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    g.Alternate = GPIO_AF2_TIM3;
    HAL_GPIO_Init(GPIOB, &g);

    uint32_t tclk = HAL_RCC_GetPCLK1Freq();
    if ((RCC->CFGR & RCC_CFGR_PPRE1) != 0) tclk *= 2;

    htim3.Instance = TIM3;
    htim3.Init.Prescaler = (tclk / 1000000U) - 1;   /* 1 MHz = 1 us tick */
    htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim3.Init.Period = 20000 - 1;                  /* 50 Hz */
    htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    HAL_TIM_PWM_Init(&htim3);

    TIM_OC_InitTypeDef oc = {0};
    oc.OCMode = TIM_OCMODE_PWM1;
    oc.Pulse = SERVO_CLOSED_US;
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;
    HAL_TIM_PWM_ConfigChannel(&htim3, &oc, TIM_CHANNEL_1);
    HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1);
}

/* ======================================================================== */
/*                       GPIO, slots, lights, housekeeping                  */
/* ======================================================================== */
static void gpio_init(void)
{
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* Inputs with pull-up: slot IRs (PA0, PA1, PA4), entrance IR (PB10) */
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    g.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_4;
    HAL_GPIO_Init(GPIOA, &g);
    g.Pin = ENT_PIN | GPIO_PIN_0;                 /* entrance IR + light-sensor module (PB0) */
    HAL_GPIO_Init(GPIOB, &g);

    /* Outputs */
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    g.Pin = LIGHT1_PIN | LIGHT2_PIN;
    HAL_GPIO_Init(GPIOA, &g);
    g.Pin = BUZZ_PIN | GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
    HAL_GPIO_Init(GPIOB, &g);

    HAL_GPIO_WritePin(BUZZ_PORT, BUZZ_PIN, GPIO_PIN_RESET);
}

static uint8_t entrance_active(void)
{
    return HAL_GPIO_ReadPin(ENT_PORT, ENT_PIN) == IR_ACTIVE_LEVEL;
}

/* Slots that are neither occupied nor reserved (shown on OLED) */
static uint8_t free_slots(void)
{
    uint8_t n = 0;
    for (int i = 0; i < 3; i++) if (!occupied[i] && !booked[i]) n++;
    return n;
}

/* Sends slot status to the ESP32 on change and every 2 s */
static void status_tx(void)
{
    static uint32_t last_tx = 0;
    static uint8_t  last_mask = 0xFF;
    uint8_t mask = (uint8_t)(occupied[0] | (occupied[1] << 1) | (occupied[2] << 2));

    if (mask != last_mask || HAL_GetTick() - last_tx >= 2000) {
        char s[] = "S,0,0,0\n";
        s[2] = (char)('0' + occupied[0]);
        s[4] = (char)('0' + occupied[1]);
        s[6] = (char)('0' + occupied[2]);
        uart_send(s);
        last_mask = mask;
        last_tx = HAL_GetTick();
    }
}

/* Reads slot sensors, drives slot LEDs and parking lights, talks to ESP32. Call often. */
static void housekeeping(void)
{
    static uint32_t last_ldr = 0;
    static uint8_t light1 = 0;

    uart_poll();

    for (int i = 0; i < 3; i++) {
        occupied[i] = (HAL_GPIO_ReadPin(SLOT_PORT[i], SLOT_PIN[i]) == IR_ACTIVE_LEVEL);
        if (assigned_at[i] && (occupied[i] || HAL_GetTick() - assigned_at[i] > ASSIGN_HOLD_MS)) assigned_at[i] = 0;
#if LED_ON_WHEN_FREE
        uint8_t on = (!occupied[i] && !booked[i]);
#else
        uint8_t on = occupied[i];
#endif
        HAL_GPIO_WritePin(GPIOB, SLOT_LED_PIN[i], on ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }

    if (HAL_GetTick() - last_ldr >= 200) {
        last_ldr = HAL_GetTick();
        light1 = (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_0) == LDR_DARK_LEVEL);   /* PB0 */
        HAL_GPIO_WritePin(LIGHT_PORT, LIGHT1_PIN, light1 ? GPIO_PIN_SET : GPIO_PIN_RESET);
        HAL_GPIO_WritePin(LIGHT_PORT, LIGHT2_PIN, light1 ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }

    status_tx();
}

/* Delay that keeps slots/LEDs/lights/UART alive */
static void delay_service(uint32_t ms)
{
    uint32_t t = HAL_GetTick();
    while (HAL_GetTick() - t < ms) { housekeeping(); HAL_Delay(5); }
}

/* ======================================================================== */
/*                               UI                                         */
/* ======================================================================== */
static uint8_t last_sig = 0xFF;

static void ui_idle(void)
{
    static const char *const txt[3][3] = {
        {"SLOT 1: FREE", "SLOT 1: RESERVED", "SLOT 1: OCCUPIED"},
        {"SLOT 2: FREE", "SLOT 2: RESERVED", "SLOT 2: OCCUPIED"},
        {"SLOT 3: FREE", "SLOT 3: RESERVED", "SLOT 3: OCCUPIED"},
    };
    char line[20] = "FREE SLOTS: ";
    line[12] = (char)('0' + free_slots());
    line[13] = 0;

    oled_clear();
    oled_str(0, 0, "SMART PARKING");
    for (int i = 0; i < 3; i++) {
        uint8_t st = occupied[i] ? 2 : (booked[i] ? 1 : 0);
        oled_str(0, 2 + i, txt[i][st]);
    }
    oled_str(0, 6, line);
    oled_update();
}

static void ui_msg(const char *l1, const char *l2, const char *l3)
{
    oled_clear();
    oled_str(0, 1, l1);
    oled_str(0, 3, l2);
    oled_str(0, 5, l3);
    oled_update();
}

/* ======================================================================== */
/*                          CARD AUTHENTICATION                             */
/* ======================================================================== */
/* Waits for a card tap. Returns 1 = authorized card, 0 = timeout / reader error */
static uint8_t auth_wait_card(uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();
    uint8_t uid[4];

    if (!rc_ok) {
        ui_msg("READER ERROR", "CHECK RC522", "");
        delay_service(2000);
        return 0;
    }

    while (HAL_GetTick() - t0 < timeout_ms) {
        housekeeping();
        if (rc_read_uid(uid)) {
            for (uint8_t i = 0; i < 4; i++) last_uid[i] = uid[i];
            rc_field_reset();                                  /* so the card can be read again */
            if (uid_authorized(uid)) return 1;

            ui_msg("WRONG CARD", "TRY AGAIN", "");
            buzzer_pattern(2, 100, 100);
            delay_service(1500);
            ui_msg("CAR DETECTED", "TAP YOUR CARD", "");
        }
        HAL_Delay(50);
    }
    return 0;
}

/* ======================================================================== */
/*                               GATE                                       */
/* ======================================================================== */
static void servo_move(int target_us)
{
    while (servo_cur_us != target_us) {
        if (servo_cur_us < target_us) {
            servo_cur_us += 20;
            if (servo_cur_us > target_us) servo_cur_us = target_us;
        } else {
            servo_cur_us -= 20;
            if (servo_cur_us < target_us) servo_cur_us = target_us;
        }
        servo_write(servo_cur_us);
        delay_service(15);
    }
}

static void gate_cycle(void)
{
    servo_move(SERVO_OPEN_US);                             /* open to 90 deg */
    ui_msg("GATE OPEN", "PLEASE ENTER", "");
    delay_service(GATE_OPEN_MS);                           /* stay open 5 s */
    ui_msg("GATE CLOSING", "", "");
    servo_move(SERVO_CLOSED_US);                           /* back to 0 deg */
}

/* ======================================================================== */
/*                          VEHICLE AT ENTRANCE                             */
/* ======================================================================== */
static void handle_vehicle(void)
{
    char line[20] = "GO TO SLOT 0";
    int free_slot = -1;      /* empty, not booked  -> opens automatically */
    int booked_slot = -1;    /* empty, booked      -> needs card authentication */

    housekeeping();                                        /* refresh slot status + bookings */

    for (int i = 0; i < 3; i++) {
        if (occupied[i] || assigned_at[i]) continue;
        if (!booked[i]) { if (free_slot < 0)   free_slot = i; }
        else            { if (booked_slot < 0) booked_slot = i; }
    }

    if (free_slot >= 0) {                                  /* automatic entry */
        buzzer_beep(100);
        line[11] = (char)('1' + free_slot);
        line[12] = 0;
        ui_msg("CAR DETECTED", "WELCOME!", line);
        delay_service(1000);
        assigned_at[free_slot] = HAL_GetTick() | 1u;
        gate_cycle();
    } else if (booked_slot >= 0) {                         /* RC522 authentication, then entry */
        buzzer_beep(100);
        ui_msg("CAR DETECTED", "TAP YOUR CARD", "");

        if (auth_wait_card(AUTH_TIMEOUT_MS)) {
            line[11] = (char)('1' + booked_slot);
            line[12] = 0;
            ui_msg("AUTHENTICATION", "DONE!", line);
            buzzer_beep(200);
            delay_service(1000);
            assigned_at[booked_slot] = HAL_GetTick() | 1u;
            gate_cycle();
        } else {
            ui_msg("NOT AUTHENTICATED", "GATE CLOSED", "");
            buzzer_pattern(2, 600, 200);
            delay_service(2000);
        }
    } else {
        ui_msg("NO SLOT", "PARKING FULL!", "GATE CLOSED");
        buzzer_pattern(2, 600, 200);
        delay_service(2500);
    }

    /* Do not re-trigger while the same car is still in front of the sensor */
    while (entrance_active()) delay_service(50);
    delay_service(1000);
    last_sig = 0xFF;                                       /* force idle screen redraw */
}

/* ======================================================================== */
/*                               MAIN                                       */
/* ======================================================================== */
int main(void)
{
    HAL_Init();
    gpio_init();
    servo_init();
    i2c_init();
    uart_init();
    oled_init();
    rc522_init();

    if (!oled_ok) buzzer_pattern(5, 50, 50);               /* OLED not found on I2C (0x3C) */
    if (!rc_ok)   buzzer_pattern(3, 150, 150);             /* RC522 not responding on SPI  */

    ui_msg("SMART PARKING", "STARTING...", "");
    buzzer_beep(300);                                      /* startup OK */

    while (1) {
        housekeeping();

        uint8_t sig = (uint8_t)(occupied[0] | (occupied[1] << 1) | (occupied[2] << 2) |
                                (booked[0] << 3) | (booked[1] << 4) | (booked[2] << 5));
        if (sig != last_sig) {
            last_sig = sig;
            ui_idle();
        }

        if (entrance_active()) {
            delay_service(100);                            /* debounce */
            if (entrance_active()) handle_vehicle();
        }
        HAL_Delay(10);
    }
}