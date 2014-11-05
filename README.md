NanOS
=====

Tiny operating system written in C++ (Object oriented) to x86 machine. 

This os was writen to get knowlage about how on the basic level os works.

This code is based on many of tutorials and documentations, and can help people whos want to start developing own os.


Project conains:

* Supports Ext2 Filesystem - *only read*
    * List directory
    * Read file
    * Read informations about files by inodes
    * Read file by path
* Supports raw communication with HDD
    * raw read
    * raw write
* Console
  * Write on screen
  * input from keyboard
  * scrooling
  * moving coursor
  * clear screen
* Interupts handling
* Timmers handling
* String class - *own implementation more similar to c#*
* List class - *own implementation more similar to c#*
* Memory manager - *important: must be changed to someting more complex*
    * alloc
    * free
    * malloc
* Support c++ operators
    * new
    * delete


System is booted with *GRUB*.

