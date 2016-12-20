#include "textpointer.h"
#include "string.h"  //strdup
#include "stdlib.h"  //free


Text::Text(): Cstr(nullptr){
  //all is well
}

Text::Text(unsigned size): Cstr( static_cast<TextKey>( calloc(size+1,1))){
  //we have allocated a buffer and filled it with 0
}

Text::Text(Text &other):Cstr(other){
  other.clear();
}

Text::Text(const char *ptr,bool takeit): Cstr( nonTrivial(ptr) ? (takeit? ptr : strdup(ptr)) : nullptr){
  //we now own what was passed, or the duplicate we created.
}

Text::~Text(){
  clear(); //using clear instead of just free as a guard against using this after it is free'd.
}

Text::operator TextKey() const {
  return Cstr::c_str();
}

void Text::take(TextKey other){
  if(ptr != other) { //# if not passed self as a pointer to this' storage.
    clear();
    ptr=other;
  }
  //else self and we already own ourself.
}

void Text::copy(TextKey other){
  if(ptr != other) { //# if not passed self as a pointer to this' storage.
    clear();
    if(nonTrivial(other)) {
      ptr = strdup(other);
    }
  } else {//pointing to our own data (or both ptr's null)
    if(nonTrivial(other)) {//definitely same data
      ptr = strdup(other);//don't delete!
    }
  }
}

TextKey Text::operator =(TextKey other){
  copy(other);
  return other;
}

void Text::clear(){
  free(const_cast<char *>(ptr));
  ptr = nullptr;
}
