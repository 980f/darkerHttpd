#ifndef MEMORYMAPPER_H
#define MEMORYMAPPER_H

////////////////////
#include "fildes.h"


class MemoryMapper {
  Fildes fd;
  MemoryMapper ();
public:
  void *map(unsigned addr, unsigned len);
  /** could be static, but we don't want to call it without the context of having accessed map, and it gives us a place to save error codes */
  bool free(void *addr, unsigned size);

  int getError();
  /**create shared one. If this fails abort your application, you can't access whatever it is  */
  static bool init(bool refresh=false);
  static MemoryMapper *Mmap;

  static bool isOperational();
};


/** wrapper class to ensure we unmap when we drop the pointer. */
template <typename Any> class Mapped {
  Any *ptr;
  unsigned quantity;
public:
  Mapped(unsigned physical,unsigned quantity):ptr(nullptr),quantity(quantity){
    if(MemoryMapper::init()){
      ptr= reinterpret_cast<Any *>(MemoryMapper::Mmap->map(physical,quantity*sizeof (Any)));
    }
    //else pointer will be null and we will get sigsegv's
    //you are expected to call MemoryMapper::init yourself and not create Mapped blocks if that fails.
  }

  ~Mapped(){
    MemoryMapper::Mmap->free(ptr,quantity*sizeof (Any));
  }

private:
  void operator =(Any *other)=delete;//do not allow
public:
  /** bad index results in whacking the 0th element, hopefully you will notice that. */
  Any &operator [](unsigned index)const {
    return ptr[index<quantity?index:0];
  }
  //no inc's, no operator =,
};

#endif // MEMORYMAPPER_H
