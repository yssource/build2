// file      : build2/test/script/script.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  namespace test
  {
    namespace script
    {
      inline command_to_stream
      operator&= (command_to_stream& x, command_to_stream y)
      {
        return x = static_cast<command_to_stream> (
          static_cast<uint16_t> (x) & static_cast<uint16_t> (y));
      }

      inline command_to_stream
      operator|= (command_to_stream& x, command_to_stream y)
      {
        return x = static_cast<command_to_stream> (
          static_cast<uint16_t> (x) | static_cast<uint16_t> (y));
      }

      inline command_to_stream
      operator& (command_to_stream x, command_to_stream y) {return x &= y;}

      inline command_to_stream
      operator| (command_to_stream x, command_to_stream y) {return x |= y;}

      inline ostream&
      operator<< (ostream& o, const command& c)
      {
        to_stream (o, c, command_to_stream::all);
        return o;
      }
    }
  }
}