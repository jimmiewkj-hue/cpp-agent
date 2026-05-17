#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace agent {
namespace infra {
namespace protolite {

enum class WireType {
  Varint = 0,
  LengthDelimited = 2
};

struct Field {
  int number = 0;
  WireType wireType = WireType::Varint;
  std::uint64_t varintValue = 0;
  std::string lengthDelimitedValue;
};

class Reader {
 public:
  explicit Reader(const std::string& data);

  bool ReadField(Field* field);
  bool ok() const;
  bool eof() const;

 private:
  bool ReadVarint(std::uint64_t* value);
  bool ReadLengthDelimited(std::string* value);

  const std::string& data_;
  std::size_t cursor_ = 0;
  bool ok_ = true;
};

void WriteVarint(std::string* output, std::uint64_t value);
void WriteKey(std::string* output, int fieldNumber, WireType wireType);
void WriteInt32(std::string* output, int fieldNumber, int value);
void WriteInt64(std::string* output, int fieldNumber, long long value);
void WriteBool(std::string* output, int fieldNumber, bool value);
void WriteString(std::string* output, int fieldNumber, const std::string& value);
void WriteBytes(std::string* output, int fieldNumber, const std::string& value);
void WriteMessage(
    std::string* output,
    int fieldNumber,
    const std::string& messageBytes);

bool FieldToInt32(const Field& field, int* value);
bool FieldToInt64(const Field& field, long long* value);
bool FieldToBool(const Field& field, bool* value);
bool FieldToString(const Field& field, std::string* value);

}  // namespace protolite
}  // namespace infra
}  // namespace agent
