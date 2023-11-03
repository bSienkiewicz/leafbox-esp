#include <WiFi.h>
#include <Arduino.h>

#define uS_TO_S_FACTOR 1000000

esp_sleep_wakeup_cause_t get_wakeup_reason(){
  esp_sleep_wakeup_cause_t wakeup_reason;
  wakeup_reason = esp_sleep_get_wakeup_cause();
  switch(wakeup_reason)
  {
      case ESP_SLEEP_WAKEUP_EXT0 : Serial.println("Wakeup caused by external signal using RTC_IO"); break;
      case ESP_SLEEP_WAKEUP_EXT1 : Serial.println("Wakeup caused by external signal using RTC_CNTL"); break;
      case ESP_SLEEP_WAKEUP_TIMER : Serial.println("Wakeup caused by timer"); break;
      case ESP_SLEEP_WAKEUP_TOUCHPAD : Serial.println("Wakeup caused by touchpad"); break;
      case ESP_SLEEP_WAKEUP_ULP : Serial.println("Wakeup caused by ULP program"); break;
      default : Serial.printf("Wakeup was not caused by deep sleep: %d\n",wakeup_reason); break;
  }

  return wakeup_reason;
}

void sleep_for(int seconds) {
  delay(200);
  WiFi.mode(WIFI_OFF);
  delay(200);
  esp_sleep_enable_timer_wakeup(seconds * uS_TO_S_FACTOR);
  Serial.println("Going to sleep for " + String(seconds) + " seconds");
  esp_deep_sleep_start();
}