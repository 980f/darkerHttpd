#include <iostream>
using namespace std;

#include "streamprintf.h"

int testPrinter(int argc, char *argv[]){
  StreamPrintf pf(cout);
  unsigned which=(argc>1)?atoi(argv[1]):BadIndex;
  if(isValid(which++)){
    if(which<argc){
      pf(argv[which],1,2.3,"tree",-4.1);
    } else {
      cerr<<"Bad test selection index"<<endl;
    }
  } else {
    for(which=argc;which-->2;){//argv0 is command, argv.1 is which index
      pf(argv[which],1,2.3,"tree",-4.1);
    }
  }
  cerr << endl;
  return 0;
}


int testScanner(int argc,char *argv[]){
  int integer=42;
  double dee=3.14159;
  char see='B';


  unsigned which=(argc>1)?atoi(argv[1]):BadIndex;

  StreamScanf sf(cin);

  if(isValid(which++)){
    if(which<argc){
      sf(argv[which],&see,&integer,&dee);
      cout<<integer<<","<<dee<<","<<see<<endl;
    } else {
      cerr<<"Bad test selection index"<<endl;
    }
  } else {
    for(which=argc;which-->2;){//argv0 is command, argv.1 is which index
      sf(argv[which],&see,&integer,&dee);
      cout<<integer<<","<<dee<<","<<see<<endl;
    }
  }
  cerr << endl;
  return -2;
}

#include "hook.h"
//all of the 3 following lines worked :)
//Hooker<int,int,char*[]> Mainly(5,testScanner);
Hooker<int,int,char*[]> Mainly(5,testPrinter);
//Hooker<int,int,char*[]> Mainly(5);


int main(int argc, char *argv[]){
// return testPrinter(argc,argv);
// return testScanner(argc,argv);
  int report= Mainly(argc,argv);
  return report;
}
