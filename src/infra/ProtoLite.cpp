#include "infra/ProtoLite.h"

#include <limits>

namespace agent {
namespace infra {
namespace protolite {

namespace {

bool CheckedCastU64ToInt32(std::uint64_t value, int* out) {
  if (out == nullptr ||
      value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  *out = static_cast<int>(value);
  return true;
}

}  // namespace

Reader::Reader(const std::string& data) : data_(data) {}

bool Reader::ReadField(Field* field) {
  if (field == nullptr) {
    ok_ = false;
    return false;
  }
  if (!ok_ || cursor_ >= data_.size()) {
    return false;
  }

  std::uint64_t key = 0;
  if (!ReadVarint(&key)) {
    ok_ = false;
    return false;
  }

  field->number = static_cast<int>(key >> 3);
  field->wireType = static_cast<WireType>(key & 0x07);
  field->varintValue = 0;
  field->lengthDelimitedValue.clear();

  if (field->number <= 0) {
    ok_ = false;
    return false;
  }

  switch (field->wireType) {
    case WireType::Varint:
      return ReadVarint(&field->varintValue);
    case WireType::LengthDelimited:
      return ReadLengthDelimited(&field->lengthDelimitedValue);
    default:
      ok_ = false;
      return false;
  }
}

bool Reader::ok() const { return ok_; }

bool Reader::eof() const { return cursor_ >= data_.size(); }

bool Reader::ReadVarint(std::uint64_t* value) {
  if (value == nullptr) {
    ok_ = false;
    return false;
  }
  std::uint64_t result = 0;
  int shift = 0;
  while (cursor_ < data_.size() && shift <= 63) {
    const unsigned char byte =
        static_cast<unsigned char>(data_[cursor_++]);
    result |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      *value = result;
      return true;
    }
    shift += 7;
  }
  ok_ = false;
  return false;
}

bool Reader::ReadLengthDelimited(std::string* value) {
  if (value == nullptr) {
    ok_ = false;
    return false;
  }
  std::uint64_t length = 0;
  if (!ReadVarint(&length)) {
    return false;
  }
  if (length > static_cast<std::uint64_t>(data_.size() - cursor_)) {
    ok_ = false;
    return false;
  }
  value->assign(data_.data() + cursor_, data_.data() + cursor_ + length);
  cursor_ += static_cast<std::size_t>(length);
  return true;
}

void WriteVarint(std::string* output, std::uint64_t value) {
  if (output == nullptr) {
    return;
  }
  while (value >= 0x80) {
    output->push_back(static_cast<char>((value & 0x7F) | 0x80));
    value >>= 7;
  }
  output->push_back(static_cast<char>(value));
}

void WriteKey(std::string* output, int fieldNumber, WireType wireType) {
  if (output == nullptr || fieldNumber <= 0) {
    return;
  }
  const std::uint64_t key =
      (static_cast<std::uint64_t>(fieldNumber) << 3) |
      static_cast<std::uint64_t>(wireType);
  WriteVarint(output, key);
}

void WriteInt32(std::string* output, int fieldNumber, int value) {
  WriteKey(output, fieldNumber, WireType::Varint);
  WriteVarint(output, static_cast<std::uint64_t>(
                          static_cast<std::uint32_t>(value)));
}

void WriteInt64(std::string* output, int fieldNumber, long long value) {
  WriteKey(output, fieldNumber, WireType::Varint);
  WriteVarint(output, static_cast<std::uint64_t>(value));
}

void WriteBool(std::string* output, int fieldNumber, bool value) {
  WriteKey(output, fieldNumber, WireType::Varint);
  WriteVarint(output, value ? 1 : 0);
}

void WriteString(
    std::string* output,
    int fieldNumber,
    const std::string& value) {
  WriteBytes(output, fieldNumber, value);
}

void WriteBytes(
    std::string* output,
    int fieldNumber,
    const std::string& value) {
  WriteKey(output, fieldNumber, WireType::LengthDelimited);
  WriteVarint(output, static_cast<std::uint64_t>(value.size()));
  output->append(value);
}

void WriteMessage(
    std::string* output,
    int fieldNumber,
    const std::string& messageBytes) {
  WriteBytes(output, fieldNumber, messageBytes);
}

bool FieldToInt32(const Field& field, int* value) {
  if (field.wireType != WireType::Varint) {
    return false;
  }
  return CheckedCastU64ToInt32(field.varintValue, value);
}

bool FieldToInt64(const Field& field, long long* value) {
  if (field.wireType != WireType::Varint || value == nullptr ||
      field.varintValue >
          static_cast<std::uint64_t>(std::numeric_limits<long long>::max())) {
    return false;
  }
  *value = static_cast<long long>(field.varintValue);
  return true;
}

bool FieldToBool(const Field& field, bool* value) {
  if (field.wireType != WireType::Varint || value == nullptr) {
    return false;
  }
  *value = field.varintValue != 0;
  return true;
}

bool FieldToString(const Field& field, std::string* value) {
  if (field.wireType != WireType::LengthDelimited || value == nullptr) {
    return false;
  }
  *value = field.lengthDelimitedValue;
  return true;
}

}  // namespace protolite
}  // namespace infra
}  // namespace agent
