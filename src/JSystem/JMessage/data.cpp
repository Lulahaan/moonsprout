#include "JSystem/JMessage/data.h"

namespace JMessage {
namespace data {
const int ga4cSignature       = PC_SWITCH(__builtin_bswap32('MESG'), 'MESG');
const int ga4cSignature_color = PC_SWITCH(__builtin_bswap32('MGCL'), 'MGCL');
} // namespace data
} // namespace JMessage
