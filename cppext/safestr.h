#ifndef SAFESTR_H
#define SAFESTR_H
/** a string that it is hard to go past the end of. 
  * allocate a buffer and a pointer manager for it.
  */

#include "eztypes.h"
#include "charformatter.h"
template <int Size> class SafeStr: public CharFormatter {
  char content[Size];
public:
  SafeStr(void): CharFormatter(content, Size){
    //#nada
  }
  /** @returns copy constructor usable clone of this*/
  CharScanner contents(){
    return CharScanner(*this);
  }

  void restore(){
    wrap(content, Size);
  }

  /** first critical use in Storable::clone */
  template <int Othersize> void operator =(SafeStr<Othersize> &other){
    rewind();
    cat(other.asciiz());
  }

  bool matches(const char *s){
    if(ordinal() == Size) {
      return CharScanner::matches(s);
    } else {
      return *this == s;
    }
  }

  const char *c_str(){
    return asciiz();
  }

};

#endif // SAFESTR_H
