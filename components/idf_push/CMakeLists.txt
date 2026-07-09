idf_component_register(
  SRCS "idf_push.cpp"
  INCLUDE_DIRS "include"
  REQUIRES
    esp_http_client
    esp_timer
    esp-tls
    idf_config
    idf_inbox
    idf_logbuf
    idf_modem
    idf_wifi
    lwip
    mbedtls
)
