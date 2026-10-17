# ESP32 Environmental Monitor — FreeRTOS

Real-time environmental monitoring system built on ESP32 
using FreeRTOS with 5 concurrent tasks.

## Features
- Temperature & Humidity monitoring (DHT22)
- Air Quality monitoring (MQ135)
- Live OLED display (SSD1306)
- Real-time alerts via buzzer and LED
- Wi-Fi MQTT data publishing
- Node-RED live dashboard

## FreeRTOS Concepts Used
- Tasks (5 concurrent)
- Mutex (shared data protection)
- Binary Semaphore (alert triggering)
- Task Priorities (4 levels)

## Hardware
- ESP32 DevKit V1
- DHT22 Temperature/Humidity Sensor
- MQ135 Air Quality Sensor
- SSD1306 OLED Display (128x64)
- Active Buzzer
- Red LED

## Software
- ESP-IDF v6
- FreeRTOS (built-in)
- Mosquitto MQTT Broker
- Node-RED Dashboard
