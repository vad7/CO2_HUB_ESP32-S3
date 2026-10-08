// lgfx_board.h — конфигурация LovyanGFX под выбранную плату (пины из config.h)
#pragma once
#include "lgfx_include.h"
#include "config.h"

class LGFX : public lgfx::LGFX_Device {
#if defined(BOARD_ES3C28P)
    lgfx::Panel_ILI9341 _panel;
    lgfx::Bus_SPI       _bus;
    lgfx::Light_PWM     _light;
    lgfx::Touch_FT5x06  _touch;   // FT6336G совместим с FT5x06
#elif defined(BOARD_TDISPLAY_S3)
    lgfx::Panel_ST7789  _panel;
    lgfx::Bus_Parallel8 _bus;
    lgfx::Light_PWM     _light;
#endif

public:
    LGFX()
    {
        {
            auto cfg = _bus.config();
#if defined(BOARD_ES3C28P)
            cfg.spi_host    = SPI2_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = LCD_SPI_HZ;
            cfg.freq_read   = 16000000;
            cfg.spi_3wire   = false;
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk    = LCD_PIN_SCK;
            cfg.pin_mosi    = LCD_PIN_MOSI;
            cfg.pin_miso    = LCD_PIN_MISO;
            cfg.pin_dc      = LCD_PIN_DC;
#elif defined(BOARD_TDISPLAY_S3)
            cfg.freq_write  = LCD_BUS_HZ;
            cfg.pin_wr      = LCD_PIN_WR;
            cfg.pin_rd      = LCD_PIN_RD;
            cfg.pin_rs      = LCD_PIN_DC;
            cfg.pin_d0 = LCD_PIN_D0; cfg.pin_d1 = LCD_PIN_D1; cfg.pin_d2 = LCD_PIN_D2; cfg.pin_d3 = LCD_PIN_D3;
            cfg.pin_d4 = LCD_PIN_D4; cfg.pin_d5 = LCD_PIN_D5; cfg.pin_d6 = LCD_PIN_D6; cfg.pin_d7 = LCD_PIN_D7;
#endif
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs        = LCD_PIN_CS;
            cfg.pin_rst       = LCD_PIN_RST;
            cfg.pin_busy      = -1;
            cfg.panel_width   = LCD_WIDTH;
            cfg.panel_height  = LCD_HEIGHT;
            cfg.offset_x      = LCD_OFFSET_X;
            cfg.offset_y      = LCD_OFFSET_Y;
            cfg.offset_rotation = 0;
            cfg.readable      = false;
            cfg.invert        = LCD_INVERT;
            cfg.rgb_order     = false;
            cfg.dlen_16bit    = false;
            cfg.bus_shared    = false;
            _panel.config(cfg);
        }
        {
            auto cfg = _light.config();
            cfg.pin_bl      = LCD_PIN_BL;
            cfg.invert      = false;
            cfg.freq        = 44100;
            cfg.pwm_channel = 7;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
#if HAS_TOUCH
        {
            auto cfg = _touch.config();
            cfg.x_min      = 0;
            cfg.x_max      = LCD_WIDTH - 1;
            cfg.y_min      = 0;
            cfg.y_max      = LCD_HEIGHT - 1;
            cfg.pin_int    = TOUCH_PIN_INT;
            cfg.pin_rst    = TOUCH_PIN_RST;
            cfg.bus_shared = false;
            cfg.offset_rotation = 0;   // если касания зеркальны/повёрнуты — подобрать 0..7
            cfg.i2c_port   = I2C_PORT;
            cfg.i2c_addr   = TOUCH_I2C_ADDR;
            cfg.pin_sda    = I2C_PIN_SDA;
            cfg.pin_scl    = I2C_PIN_SCL;
            cfg.freq       = K22_I2C_HZ;   // общая шина с K22: не выше 100 кГц
            _touch.config(cfg);
            _panel.setTouch(&_touch);
        }
#endif
        setPanel(&_panel);
    }
};
