set(srcs)

list(APPEND srcs "MQTTClient.cpp")


idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       REQUIRES esp-eCommon mbedtls
                      )

