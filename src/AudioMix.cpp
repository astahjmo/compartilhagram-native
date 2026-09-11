#include "SystemAudio.h"
#include <QtEndian>
#include <algorithm>

QByteArray SystemAudio::mixPcm(const QList<QByteArray> &inputs) {
  QByteArray out(1920, '\0'); // 10 ms, 48 kHz, stereo, signed 16-bit LE.
  for (int sample = 0; sample < 960; ++sample) {
    int32_t sum = 0;
    for (const auto &pcm : inputs)
      if (pcm.size() >= (sample + 1) * 2)
        sum += qFromLittleEndian<int16_t>(pcm.constData() + sample * 2);
    qToLittleEndian<int16_t>(std::clamp(sum, -32768, 32767),
                             out.data() + sample * 2);
  }
  return out;
}
