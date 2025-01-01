#include "minimath.h"

#include <array>
#include <cheaptricks.h>

#ifdef __linux__
#include <limits>
const double Infinity = std::numeric_limits<double>::infinity();
const double Nan = std::numeric_limits<double>::quiet_NaN();

unsigned log2Exponent(uint32_t number) {
  //can be really fast in asm
  for (uint32_t exp = 0; exp < 32; ++exp) {
    if (number) {
      number >>= 1;
    } else {
      return exp;
    }
  }
  return 32;
} // log2Exponent


#else // ifdef __linux__
//firmware platform didn't have a useful limits.h so ...
static int64_t InfPattern = 0x7FFLL << 52;
static int64_t NanPattern = 0x7FF8LL << 48;

const double Infinity = pun(double,InfPattern);
const double Nan(pun(double,NanPattern));


constexpr bool isSignal(double d){
  return d == Nan || d == Infinity;
}

constexpr bool isNan(double arg){
  return arg == Nan;
}

constexpr bool isNormal(double d){//mimicing std::isnormal which means 'is fully normalized fp number'
  return d!=0 && !isSignal(d);
}

#endif // ifdef __linux__

//powers of 10 that fit into a 32 bit integer
constexpr uint32_t Decimal1[] = {
  1,
  10,
  100,
  1000,
  10'000,
  100'000,
  1'000'000,
  10'000'000,
  100'000'000,
  1'000'000'000
};

// powers of 10 that fit into a 64 bit integer, but not a 32bit integer.
constexpr uint64_t Decimal2[] = {
  10'000'000'000UL,
  100'000'000'000UL,
  1'000'000'000'000UL,
  10'000'000'000'000UL,
  100'000'000'000'000UL,
  1'000'000'000'000'000UL,
  10'000'000'000'000'000UL,
  100'000'000'000'000'000UL,
  1'000'000'000'000'000'000UL,
  10'000'000'000'000'000'000UL //10^19,
  //compiler reported overflow when I added one more.
};


template<typename Integrish> constexpr Integrish rpepow10(unsigned n) {
  Integrish result = 1;
  Integrish based = 10;
  for (; n; n >>= 1) {
    if (n & 1) result *= based;
    based *= based;
  }
  return result;
}

template<class Function, std::size_t... domain> constexpr auto MakeLookUpTable(Function f, std::index_sequence<domain...>) -> std::array<typename std::result_of<Function(unsigned)>::type, sizeof...(domain)> {
  return {{f(domain)...}};
}

constexpr std::array<unsigned, 10> tabull = MakeLookUpTable(rpepow10<unsigned>, std::make_index_sequence<10>());
constexpr std::array<uint64_t, 10> tabull2 = MakeLookUpTable(rpepow10<uint64_t>, std::make_index_sequence<10>());//need 10 to 19, not 0 to 9


unsigned i32pow10(unsigned power) {
  if (power < countof(Decimal1)) {
    return Decimal1[power];
  }
  return 0; //this should get the caller's attention.
}

uint64_t i64pow10(unsigned power) {
  if (power >= countof(Decimal1)) {
    power -= countof(Decimal1);
    if (power < countof(Decimal2)) {
      return Decimal2[power];
    }
    return 0; //overflow
  } else {
    return Decimal1[power];
  }
} // i64pow10


/** @returns the number of decimal digits needed to represent the given integer, -1 if the number is 0
 * If the chip has a single cycle 32 bit multiplier we could do this slightly faster by generating the powers in the loop rather than iterating over a table of them, although it might be equal rather than faster.
 * But the tables are a significant boost for the pow10 functions when we don't have a multiplier (cortex M0), but then again if we are using one of those would we ever care about speed?
 */
int ilog10(uint32_t value) {
  for (int log = countof(Decimal1); log-- > 0;) {
    if (Decimal1[log] <= value) {
      return log;
    }
  }
  return -1;
}

/** @returns the number of decimal digits needed to represent the given integer, -1 if the number is 0 */
int ilog10(uint64_t value) {
  for (int log = countof(Decimal2); log-- > 0;) {
    if (Decimal2[log] <= value) {
      return log + 10;
    }
  }
  return ilog10(uint32_t(value));
}


uint64_t keepDecimals(uint64_t p19, unsigned digits) {
  return rate(p19, i64pow10(19 - digits));
}

uint64_t truncateDecimals(uint64_t p19, unsigned digits) {
  if (digits <= 19) {
    return p19 / i64pow10(19 - digits);
  }
  return 0;
}

//uround and sround are coded to be like they will in optimized assembly
uint16_t uround(float scaled) {
  if (scaled < 0.5F) { //fp compares are the same cost as integer.
    return 0;
  }
  scaled *= 2; //expose rounding bit
  if (scaled >= 131071) {
    return 65535;
  }
  int eye = int(scaled); //truncate
  return uint16_t(eye / 2);
} /* uround */

int16_t sround(float scaled) { //#this would be so much cleaner and faster in asm!
  if (scaled > 32766.5F) {
    return 32767;
  }
  if (scaled < -32767.5F) {
    return -32768;
  }
  scaled += scaled >= 0 ? 0.5 : -0.5; //round away from 0. aka round the magnitude.
  return int16_t(scaled);
}

unsigned modulus(int value, unsigned cycle) {
  if (cycle <= 1) {
    return value; //GIGO
  }
  /* since most use cases are within one cycle we use add/sub rather than try to make divide work.*/
  while (value < 0) {
    value += cycle;
  }
  while (unsigned(value) >= cycle) {
    value -= cycle;
  }
  return value;
} // modulus

unsigned saturated(unsigned quantity, double fractionThereof) {
  double dee(quantity * fractionThereof);
  if (dee < 0) {
    return 0;
  }
  unsigned rawbins(dee + 0.5);

  if (rawbins >= quantity) {
    return quantity - 1;
  } else {
    return rawbins;
  }
} /* saturated */

#undef __STRICT_ANSI__


int fexp(double d) { //todo:1 remove dependence on cmath.
  int ret = 0; //assignment required for constexpr'ness
  if (d == 0.0) { //frexp returns 0, which makes it look bigger than numbers between 0 and 1.
    return std::numeric_limits<decltype(d)>::min_exponent - 1;
    // return -1023;//one less than any non-zero number will give
  }
  frexp(d, &ret);
  return ret;
}

double dpow10(unsigned uexp) {
  if (uexp < countof(Decimal1)) {
    return double(Decimal1[uexp]);
  }
  if (uexp < countof(Decimal2) + countof(Decimal1)) {
    return double(Decimal2[uexp - countof(Decimal1)]);
  }
  return 0;
}

double dpow10(int exponent) {
  if (exponent >= 0) {
    dpow10(unsigned(exponent));
  }
  //todo: see if std lib uses RPE to compute this.
  return pow(10, exponent);
} // dpow10

//linux has this, firmware doesn't have ANY coeffs in its math.h
#ifndef M_PI
#warning platform specific value for PI
#define M_PI 3.14159265358979323846
#endif

double degree2radian(double theta) {
  return theta * (M_PI / 180);
}

#define gotFlogWorking 0

#if gotFlogWorking == 2

#ifndef M_LN2
#warning platform specific M_LN2
static const double M_LN2(0.69314718055994530942);
#endif

 double flog(uint32_t number){
  int exponent = log2Exponent(number);
  int malign = number << (30 - exponent); //unsigned 1.31

  malign = 1 << 31 - malign; //
  //goose until x is less than 1/2
  return -1.0; //not yet implemented
}

#elif gotFlogWorking == 1
static double LN2 = 0.69314718055994530942;
double flog(uint32_t number){

  static uint32_t fractroots[] = { //fractional part of the roots of 2
    0x6A09E667,
    0x306FE0A3,
    0x172B83C7,
    0xB5586CF,
    0x59B0D31,
    0x2C9A3E7,
    0x163DA9F,
    0xB1AFA5,
    0x58C86D,
    0x2C605E,
    0x162F39,
    0xB175E,
    0x58BA0,
    0x2C5CC,
    0x162E5,
    0xB172,
    0x58B9,
    0x2C5C,
    0x162E,
    0xB17,
    0x58B,
    0x2C5,
    0x162,
    0xB1,
    0x58,
    0x2C,
    0x16,
    0xB,
    0x5,
    0x2,
    0x1,
  };
  int exponent = log2Exponent(number);
  int malign = number << (31 - exponent); //actually is unsigned 1.31

  malign &= ~(1 << 31); //same as subtracting 1 in 1.31 format
  //malign is now a 0.31 format number, in the sub range of 0 to 1-2^-31.
  unsigned n = countof(fractroots);
  uint32_t logish = 0;
  uint32_t checker = 0; //we will iterate to make checker close to malign
  uint32_t *fract = fractroots;

  while(n-- > 0) { //#order matters
    uint64_t tester = *fract * checker; //best done in asm
    tester += uint64_t(*fract + checker) << 32;
    if(tester <= uint64_t(malign) << 32) {
      checker = tester >> 32;
      logish |= 1 << (n); //maybe n+1?
    }
    ++fract;
  }
  //logish is mantissa of base 2 log
  //pack fields
  uint64_t packer = uint64_t(exponent) << 52;
  packer |= logish << (52 - 32);
  return pun(double, packer) * M_LN2; //M_LN2 log base e of 2.
} /* flog */

#else /* if gotFlogWorking == 2 */
double flog(uint32_t number) {
  if (number == 0) {
    number = 1;
  }
  return log(double(number)); //without the cast the gcc-arm compiler just called __floatunsidf
}

#endif /* if gotFlogWorking == 2 */

#if logoptimized
double logRatio(uint32_t over, uint32_t under){
  unsigned underexp = 32 - log2Exponent(under); //leading zeroes
  unsigned overexp = 32 - log2Exponent(over); //leading zeroes
  int diff = overexp - underexp;
  bool negate = diff < 0;

  if(negate) { ///then ratio is greater than 1
    diff = -diff;
    under <<= diff;
  } else {
    over <<= diff;
    //swap over and under:
  }
  //could also trim leading zeroes of over.
  over = under - over; //effectively convert x to 1-y and then iterate over y.
  //now do simple series expansion using ratio powers.

} /* logRatio */

#else /* if logoptimized */
//someday we will optimize the following:
double logRatio(uint32_t over, uint32_t under) {
  return flog(over) - flog(under);
}

#endif /* if logoptimized */

/** n!/r!(n-r)! = n*(n-1)..*(n-r+1)/r*(r-1)..
 *  This is done in a complicated fashion to increase the range over what could be done if the factorials were computed then divided.
 */
unsigned Cnr(unsigned n, unsigned r) {
  if (r <= 0) { //frequent case and avert naive divide by zero
    return 1;
  }
  if (r == 1) { //fairly frequent case
    return n;
  }
  if (r == 2) {
    //divide the even number by 2, via shift.
    if (n & 1) {
      return n * ((n - 1) >> 1);
    } else {
      return (n >> 1) * (n - 1);
    }
  }

  unsigned num = n;
  unsigned denom = r;
  //optimize range by removing power of 2 from factorials while computing them
  int twos = 0;
  while (r-- > 0) {
    unsigned nterm = --n;
    while (0 == (nterm & 1)) { //50% of the time we loop just once, 25% of the time twice 12.5% of the time 3 times ...
      ++twos;
      nterm >>= 1;
    }
    num *= nterm;
    unsigned rterm = r;
    while (0 == (rterm & 1)) {
      --twos; //these discarded twos are in the denominator
      rterm >>= 1;
    }
    denom *= rterm;
  }
  //twos should be a small value
  if (twos >= 0) {
    num <<= twos;
  } else {
    denom <<= -twos;
  }
  return rate(num, denom);
} // Cnr


///** version of @see splitter that allows for long or long-long etc integer types.
unsigned digitsAbove(unsigned int value, unsigned numDigits) {
  unsigned digit = value / i32pow10(numDigits);
  value -= digit * i32pow10(numDigits);
  return digit;
}

int ilog10(double value) {
  return ilog10(uint64_t(fabs(value)));
}
