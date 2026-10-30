// 433 MHz listening mode: raw pulse capture and a generic OOK slicer.
//
// rc-switch only reports the few remote-control protocols it knows. Weather
// stations, door sensors, doorbells etc. use other encodings, so while
// listening we take the receive pin over, record every edge, cut the stream
// into packets at long gaps, throw away receiver noise and guess the
// encoding:
//   pwm  - high pulses short/long, lows constant  (most sensors, rc-switch)
//   ppm  - highs constant, low gaps short/long     (many weather stations)
//   nrz  - all widths multiples of one unit         (Manchester, FSK-ish)
// The result is a bit string as hex plus the timings, enough to recognise a
// sensor and decode it later (in the hub).
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include <algorithm>

namespace rfsniff {

static const uint16_t RING = 4096;
static const uint32_t GAP_US = 4000;   // a low this long ends a packet
static const uint16_t MIN_US = 150;    // shorter pulses are noise (the idle receiver's noise is mostly short)
static const size_t MIN_PULSES = 32;   // at least ~16 bits
static const size_t MAX_PULSES = 600;
static const size_t KEEP = 40;         // packets kept for GET /api/rf/sniff

static volatile uint16_t ring[RING];   // bit15 = level of the pulse, bits0-14 = µs (capped)
static volatile uint16_t head, tail;
static volatile uint32_t lastEdge;
static volatile bool overflow;
static int pin = -1;
static bool active;
static uint32_t until;                 // millis() when listening stops
static std::vector<uint16_t> cur;      // pulses of the packet being collected
static uint32_t seen, kept;
static volatile uint32_t edges;
static uint32_t shortPk, noisePk, gaps, glitches;  // diagnostics: why packets were dropped

struct Packet {
  String key;                          // enc + bits + hex, for repeat counting
  JsonDocument doc;
  uint32_t lastAt;
};
static std::vector<Packet> packets;

static void IRAM_ATTR isr() {
  uint32_t now = micros();
  edges++;
  uint32_t d = now - lastEdge;
  lastEdge = now;
  // the pin now shows the new level; the pulse that just ended had the other one
  uint16_t level = digitalRead(pin) ? 0 : 0x8000;
  uint16_t next = (head + 1) % RING;
  if (next == tail) {
    overflow = true;
    return;
  }
  ring[head] = level | (uint16_t)(d > 0x7fff ? 0x7fff : d);
  head = next;
}

// Width clusters: values within 30 % of a cluster's mean join it.
struct Cluster { uint32_t sum; uint16_t n; uint16_t mean() const { return sum / n; } };
static std::vector<Cluster> clusters(std::vector<uint16_t> v) {
  std::sort(v.begin(), v.end());
  std::vector<Cluster> c;
  for (uint16_t x : v) {
    if (!c.empty() && x <= c.back().mean() * 13 / 10) {
      c.back().sum += x;
      c.back().n++;
    } else {
      c.push_back({x, 1});
    }
  }
  return c;
}

static String bitsToHex(const std::vector<uint8_t> &bits) {
  String h;
  for (size_t i = 0; i < bits.size(); i += 4) {
    uint8_t n = 0;
    for (size_t j = 0; j < 4; j++) n = (n << 1) | (i + j < bits.size() ? bits[i + j] : 0);
    h += "0123456789abcdef"[n];
  }
  return h;
}

// Analyse one packet (pulses alternate levels; the first is high). Returns
// false for noise.
static bool analyse(const std::vector<uint16_t> &p, JsonDocument &out) {
  std::vector<uint16_t> hi, lo;
  for (size_t i = 0; i < p.size(); i++) {
    uint16_t w = p[i] & 0x7fff;
    (p[i] & 0x8000 ? hi : lo).push_back(w);
  }
  if (hi.size() < MIN_PULSES / 2 || lo.size() < MIN_PULSES / 2 - 1) return false;
  auto ch = clusters(hi), cl = clusters(lo);
  // a real transmitter uses a handful of widths; noise is all over the place
  auto big = [](const std::vector<Cluster> &c, size_t total) {
    size_t n = 0, in = 0;
    for (auto &k : c)
      if (k.n * 20 >= total) { n++; in += k.n; }  // clusters with >= 5 % of the pulses
    return std::make_pair(n, in);
  };
  auto [nh, inh] = big(ch, hi.size());
  auto [nl, inl] = big(cl, lo.size());
  if (nh == 0 || nl == 0 || nh > 3 || nl > 4) return false;
  if ((inh + inl) * 10 < (hi.size() + lo.size()) * 9) return false;  // < 90 % in main clusters

  auto mainOf = [](const std::vector<Cluster> &c, size_t total) {
    std::vector<Cluster> m;
    for (auto &k : c)
      if (k.n * 20 >= total) m.push_back(k);
    return m;
  };
  auto mh = mainOf(ch, hi.size()), ml = mainOf(cl, lo.size());
  std::vector<uint8_t> bits;
  const char *enc;
  uint16_t s = 0, l = 0;
  if (mh.size() == 2 && ml.size() <= 2) {  // pulse width
    enc = "pwm";
    s = mh[0].mean(), l = mh[1].mean();
    uint16_t thr = (s + l) / 2;
    for (uint16_t w : hi) bits.push_back(w > thr ? 1 : 0);
  } else if (mh.size() == 1 && ml.size() >= 2) {  // pulse distance (ignore a longer sync cluster)
    enc = "ppm";
    s = ml[0].mean(), l = ml[1].mean();
    uint16_t thr = (s + l) / 2, sync = l * 3 / 2;
    for (uint16_t w : lo)
      if (w < sync) bits.push_back(w > thr ? 1 : 0);
  } else {  // widths as multiples of the shortest unit
    enc = "nrz";
    uint16_t unit = std::min(mh[0].mean(), ml[0].mean());
    s = unit;
    for (uint16_t w : p) {
      int n = ((w & 0x7fff) + unit / 2) / unit;
      if (n < 1) n = 1;
      if (n > 8) n = 8;
      for (int k = 0; k < n; k++) bits.push_back(w & 0x8000 ? 1 : 0);
    }
  }
  if (bits.size() < 12) return false;
  out["enc"] = enc;
  out["bits"] = bits.size();
  out["hex"] = bitsToHex(bits);
  out["short"] = s;
  if (l) out["long"] = l;
  out["pulses"] = p.size();
  JsonArray w = out["timings"].to<JsonArray>();  // first pulses, signed: + high, - low
  for (size_t i = 0; i < p.size() && i < 96; i++) w.add(p[i] & 0x8000 ? (int)(p[i] & 0x7fff) : -(int)(p[i] & 0x7fff));
  return true;
}

// Finish the current packet; on a real one call emit(doc, isNew).
template <typename F> static void finish(F emit) {
  if (cur.size() > 4 && cur.size() < MIN_PULSES) shortPk++;
  if (cur.size() >= MIN_PULSES) {
    // The idle receiver never goes quiet, so a packet usually carries a few
    // noise pulses at both ends: try trimming them off. Packets start high.
    JsonDocument d;
    bool ok = false;
    for (size_t a = 0; a <= 8 && !ok; a++) {
      if (!(cur[a] & 0x8000)) continue;
      for (size_t b = 0; b <= 8 && !ok; b++) {
        if (cur.size() < a + b + MIN_PULSES) break;
        std::vector<uint16_t> p(cur.begin() + a, cur.end() - b);
        d.clear();
        ok = analyse(p, d);
      }
    }
    if (!ok) noisePk++;
    else {
      seen++;
      String key = String((const char *)d["enc"]) + "/" + String((int)d["bits"]) + "/" + String((const char *)d["hex"]);
      uint32_t now = millis();
      auto it = std::find_if(packets.begin(), packets.end(), [&](const Packet &x) { return x.key == key; });
      bool isNew = it == packets.end() || now - it->lastAt > 3000;  // repeats come within ms
      if (it == packets.end()) {
        if (packets.size() >= KEEP) packets.erase(packets.begin());
        packets.push_back({key, d, now});
        it = packets.end() - 1;
        it->doc["count"] = 0;
        kept++;
      }
      it->doc["count"] = (int)it->doc["count"] + 1;
      if (isNew) it->doc["burst"] = 0;
      it->doc["burst"] = (int)(it->doc["burst"] | 0) + 1;  // copies in this transmission
      it->doc["at"] = now;
      it->lastAt = now;
      emit(it->doc, isNew);
    }
  }
  cur.clear();
}

static void start(int rxPin, uint32_t ms) {
  pin = rxPin;
  until = millis() + ms;
  if (active) return;
  head = tail = 0;
  overflow = false;
  cur.clear();
  lastEdge = micros();
  pinMode(pin, INPUT);
  attachInterrupt(digitalPinToInterrupt(pin), isr, CHANGE);
  active = true;
}

static void stop() {
  if (!active) return;
  detachInterrupt(digitalPinToInterrupt(pin));
  active = false;
  cur.clear();
}

// Call from loop(). Returns true while listening.
template <typename F> static bool poll(F emit) {
  if (!active) return false;
  if ((int32_t)(millis() - until) > 0) {
    stop();
    return false;
  }
  if (overflow) {  // loop() was blocked too long; drop the half packet
    noInterrupts();
    tail = head;
    overflow = false;
    interrupts();
    cur.clear();
  }
  while (tail != head) {
    uint16_t v = ring[tail];
    tail = (tail + 1) % RING;
    uint16_t w = v & 0x7fff;
    if (!(v & 0x8000) && w >= GAP_US) {  // long low: end of packet
      gaps++;
      finish(emit);
      continue;
    }
    if (w < MIN_US) {
      glitches++;  // glitch: whatever was collected is not a clean packet
      finish(emit);
      continue;
    }
    cur.push_back(v);
    if (cur.size() > MAX_PULSES) cur.clear();
  }
  // nothing for a while after a burst: close the packet
  if (!cur.empty() && micros() - lastEdge > GAP_US * 2) finish(emit);
  return true;
}

static void status(JsonDocument &d) {
  d["on"] = active;
  d["left"] = active ? (int32_t)(until - millis()) / 1000 : 0;
  d["seen"] = seen;
  d["unique"] = kept;
  d["edges"] = edges;
  d["gaps"] = gaps;
  d["glitches"] = glitches;
  d["short"] = shortPk;
  d["noise"] = noisePk;
}

static void list(JsonDocument &d) {
  status(d);
  JsonArray a = d["packets"].to<JsonArray>();
  for (auto it = packets.rbegin(); it != packets.rend(); ++it) a.add(it->doc);
}

}  // namespace rfsniff
