// Parser de tramas del lector YRM10002 (protocolo BB type cmd lenH lenL data checksum 7E).
//
// Es el mismo algoritmo de EmisorC6.ino original (sincronía en 0xBB, longitud = 7 + len,
// checksum = suma de bytes type..data, terminador 0x7E, EPC por PC>>11), extraído a una
// clase para poder validarlo sin hardware (tests/protocol_selftest.cpp). Añadidos respecto
// al original, solo de validación: longitud mínima de datos y EPC acotado dentro de la trama.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace yrm {

constexpr size_t FRAME_MAX = 128;   // igual que el buffer original
constexpr size_t EPC_MAX = 32;      // debe coincidir con rfidzb::EPC_MAX

enum class Event : uint8_t {
  NONE = 0,
  TAG,            // trama de inventario válida: ver tag()
  READER_ERROR,   // trama 0x01/0xFF válida: ver errorCode()
  BAD_CHECKSUM,
  BAD_FRAME,      // sin terminador 0x7E, o datos de inventario inconsistentes
  OTHER           // trama válida de otro tipo (ignorada)
};

struct Tag {
  uint8_t epc[EPC_MAX];
  uint8_t epcLen;
  int8_t rssi;     // byte crudo del lector como int8 (el original lo imprime como dBm)
};

class Parser {
public:
  constexpr Parser() : frame_{}, pos_(0), expected_(0), tag_{}, errorCode_(0) {}

  // Alimenta un byte. Devuelve NONE mientras la trama no está completa.
  constexpr Event feed(uint8_t b) {
    // Buscar inicio de trama
    if (pos_ == 0 && b != 0xBB) return Event::NONE;

    frame_[pos_++] = b;

    // Al recibir los primeros 5 bytes conocemos la longitud
    if (pos_ == 5) {
      const uint16_t dataLength = static_cast<uint16_t>((static_cast<uint16_t>(frame_[3]) << 8) | frame_[4]);
      expected_ = 7 + dataLength;
      if (expected_ > FRAME_MAX) reset();
    }

    Event ev = Event::NONE;
    // Trama completa
    if (expected_ > 0 && pos_ == expected_) {
      ev = (frame_[pos_ - 1] == 0x7E) ? process() : Event::BAD_FRAME;
      reset();
    }

    // Protección del buffer
    if (pos_ >= FRAME_MAX) reset();
    return ev;
  }

  constexpr const Tag &tag() const { return tag_; }
  constexpr uint8_t errorCode() const { return errorCode_; }

private:
  constexpr void reset() {
    pos_ = 0;
    expected_ = 0;
  }

  constexpr Event process() {
    if (frame_[0] != 0xBB) return Event::BAD_FRAME;
    const uint8_t type = frame_[1];
    const uint8_t cmd = frame_[2];

    // Validar checksum (type..data)
    uint8_t checksum = 0;
    for (size_t i = 1; i < expected_ - 2; i++) checksum = static_cast<uint8_t>(checksum + frame_[i]);
    if (checksum != frame_[expected_ - 2]) return Event::BAD_CHECKSUM;

    if (type == 0x02 && cmd == 0x22) {
      // datos = RSSI(1) PC(2) EPC(n) CRC16(2)  -> al menos 5 bytes
      if (expected_ < 7 + 5) return Event::BAD_FRAME;
      const uint16_t pc = static_cast<uint16_t>((static_cast<uint16_t>(frame_[6]) << 8) | frame_[7]);
      // PC indica la longitud del EPC en palabras de 16 bits
      const size_t epcLength = ((pc >> 11) & 0x1F) * 2;
      // El EPC debe terminar antes de CRC16(2) + checksum(1) + 0x7E(1)
      if (epcLength == 0 || epcLength > EPC_MAX || 8 + epcLength > expected_ - 4) return Event::BAD_FRAME;
      tag_.rssi = static_cast<int8_t>(frame_[5]);
      tag_.epcLen = static_cast<uint8_t>(epcLength);
      for (size_t i = 0; i < EPC_MAX; i++) tag_.epc[i] = (i < epcLength) ? frame_[8 + i] : 0;
      return Event::TAG;
    }
    if (type == 0x01 && cmd == 0xFF) {
      errorCode_ = (expected_ > 7) ? frame_[5] : 0;
      return Event::READER_ERROR;
    }
    return Event::OTHER;
  }

  uint8_t frame_[FRAME_MAX];
  size_t pos_;
  size_t expected_;
  Tag tag_;
  uint8_t errorCode_;
};

}  // namespace yrm
