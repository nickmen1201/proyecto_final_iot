#pragma once

// ---------------------------------------------------------------------------
// PARAMETROS DE RADIO COMPARTIDOS
// Los incluyen el emisor y el gateway, asi que siempre coinciden. Si un
// parametro difiere entre los dos, la trama sale igual y endPacket() dice que
// fue bien, pero el gateway no recibe nada y nada lo delata.
//
// Sync word propio (no el 0xAB del gateway del curso) para que nuestro gateway
// no reciba las tramas de los demas equipos del laboratorio, ni ellos las
// nuestras. Evitar 0x34: es el de LoRaWAN publico.
//
// Ahora controlamos los dos extremos, asi que el CRC va activado: el gateway
// descarta las tramas corrompidas en vez de reenviar JSON roto a la nube.
// ---------------------------------------------------------------------------
#define RADIO_FREQ_MHZ              915.0
#define RADIO_BW_KHZ                125.0
#define RADIO_SF                    9
#define RADIO_CR                    5       // coding rate 4/RADIO_CR
#define RADIO_PREAMBULO             8
#define RADIO_SYNCWORD              0x5A
#define RADIO_POTENCIA_DBM          17

// El SX1276 no transmite mas de 255 bytes por paquete.
#define LORA_MAX_PAYLOAD            255

#include <LoRa.h>
#include "LoRaBoards.h"

#if !defined(USING_SX1276) && !defined(USING_SX1278)
#error "La libreria LoRa solo soporta SX1276/78. Revisar la placa en lib/LoRaBoards/utilities.h"
#endif

// Configura la radio con los parametros de arriba. Devuelve false si el chip no
// responde.
static inline bool iniciarRadio()
{
#ifdef RADIO_TCXO_ENABLE
    pinMode(RADIO_TCXO_ENABLE, OUTPUT);
    digitalWrite(RADIO_TCXO_ENABLE, HIGH);
#endif

    LoRa.setPins(RADIO_CS_PIN, RADIO_RST_PIN, RADIO_DIO0_PIN);
    if (!LoRa.begin(RADIO_FREQ_MHZ * 1000000)) {
        return false;
    }

    LoRa.setTxPower(RADIO_POTENCIA_DBM);
    LoRa.setSignalBandwidth(RADIO_BW_KHZ * 1000);
    LoRa.setSpreadingFactor(RADIO_SF);
    LoRa.setCodingRate4(RADIO_CR);
    LoRa.setPreambleLength(RADIO_PREAMBULO);
    LoRa.setSyncWord(RADIO_SYNCWORD);
    LoRa.enableCrc();
    LoRa.disableInvertIQ();
    return true;
}

static inline void mostrarConfigRadio()
{
    Serial.println("=========== CONFIGURACION DE RADIO ===========");
    Serial.printf("  Frecuencia     : %.1f MHz\n", RADIO_FREQ_MHZ);
    Serial.printf("  Ancho de banda : %.1f kHz\n", RADIO_BW_KHZ);
    Serial.printf("  Spreading      : SF%d\n",     RADIO_SF);
    Serial.printf("  Coding rate    : 4/%d\n",     RADIO_CR);
    Serial.printf("  Preambulo      : %d simbolos\n", RADIO_PREAMBULO);
    Serial.printf("  Sync word      : 0x%02X\n",   RADIO_SYNCWORD);
    Serial.println("  CRC            : activado");
    Serial.printf("  Potencia TX    : %d dBm\n",   RADIO_POTENCIA_DBM);
    Serial.println("==============================================");
}
