//===- LangStream.h - The raw_ostream for writing C/C++/Verilog -*- C++ -*-===//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the raw_ostream named lang_raw_ostream for writing
// C/C++/Verilog. The feature of lang_raw_ostream including:
//   Auto-indentation
//   ...
//
//===----------------------------------------------------------------------===//

#ifndef HAA_LANG_STREAM_H
#define HAA_LANG_STREAM_H

#include "llvm/Support/FormattedStream.h"

namespace llvm {
template<class LangTraits>
class lang_raw_ostream : public raw_ostream {
protected:
  // Current block indent.
  unsigned Indent;
  // Are we start form a new line?
  bool newline;
  /// TheStream - The real stream we output to. We set it to be
  /// unbuffered, since we're already doing our own buffering.
  ///
  raw_ostream *TheStream;

  /// DeleteStream - Do we need to delete TheStream in the
  /// destructor?
  ///
  bool DeleteStream;

  static bool isPrepProcChar(char C) {
    return C == LangTraits::PrepProcChar;
  }

  static bool isNewLine(const char *Ptr) {
    return *Ptr == '\n' || *Ptr == '\r';
  }

  //
  size_t line_length(const char *Ptr, size_t Size) {
    size_t Scanned = 0;
    const char *End = Ptr + Size;
    for (; Ptr != End; ++Ptr) {
      ++Scanned;
      if (isNewLine(Ptr)) return Scanned;
    }

    return Size;
  }

  virtual void write_impl(const char *Ptr, size_t Size) LLVM_OVERRIDE {
    assert(Size > 0 && "Unexpected writing zero char!");

    //Do not indent the preprocessor directive.
    if (newline && !(isPrepProcChar(*Ptr)) && Indent)
      TheStream->indent(Indent);

    size_t line_len = line_length(Ptr, Size);
    const char *NewLineStart = Ptr + line_len;

    newline = isNewLine(NewLineStart - 1);

    // Write current line.
    TheStream->write(Ptr, line_len);

    // Write the rest lines.
    if (size_t SizeLeft = Size - line_len)
      write_impl(NewLineStart, SizeLeft);
  }

  /// current_pos - Return the current position within the stream,
  /// not counting the bytes currently in the buffer.
  virtual uint64_t current_pos() const LLVM_OVERRIDE {
    // Our current position in the stream is all the contents which have been
    // written to the underlying stream (*not* the current position of the
    // underlying stream).
    return TheStream->tell();
  }

  void releaseStream() {
    // Delete the stream if needed. Otherwise, transfer the buffer
    // settings from this raw_ostream back to the underlying stream.
    if (!TheStream)
      return;
    if (DeleteStream)
      delete TheStream;
    else if (size_t BufferSize = GetBufferSize())
      TheStream->SetBufferSize(BufferSize);
    else
      TheStream->SetUnbuffered();
  }

public:
  explicit lang_raw_ostream(unsigned Ind = 0)
    : raw_ostream(), Indent(Ind), newline(true), TheStream(0),
      DeleteStream(false) {}

  explicit lang_raw_ostream(raw_ostream &Stream, bool Delete = false,
                            unsigned Ind = 0)
    : raw_ostream(), Indent(Ind), newline(true), TheStream(&Stream),
      DeleteStream(false) {
    setStream(Stream, Delete);
  }

  ~lang_raw_ostream() {
    flush();
    releaseStream();
  }

  void setStream(raw_ostream &Stream, bool Delete = false) {
    releaseStream();

    TheStream = &Stream;
    DeleteStream = Delete;

    // This formatted_raw_ostream inherits from raw_ostream, so it'll do its
    // own buffering, and it doesn't need or want TheStream to do another
    // layer of buffering underneath. Resize the buffer to what TheStream
    // had been using, and tell TheStream not to do its own buffering.
    if (size_t BufferSize = TheStream->GetBufferSize())
      SetBufferSize(BufferSize);
    else
      SetUnbuffered();
    TheStream->SetUnbuffered();
  }

  template<typename PosfixT>
  lang_raw_ostream &enter_block(PosfixT Posfix,
                                const char *Begin = LangTraits::getBlockBegin())
  {
    // flush the buffer.
    flush();
    // Increase the indent.
    Indent += 2;

    // Write the block begin character.
    operator<<(Begin);
    write(' ');
    operator<<(Posfix);

    return *this;
  }

  lang_raw_ostream &enter_block(const char *Posfix = "\n") {
    return enter_block<const char*>(Posfix);
  }

  template<typename PosfixT>
  lang_raw_ostream &exit_block(PosfixT Posfix,
                               const char *End = LangTraits::getBlockEnd()) {
    // flush the buffer.
    flush();

    assert(Indent >= 2 && "Unmatch block_begin and exit_block!");
    // Decrease the indent.
    Indent -= 2;

    // Write the block begin character.
    operator<<(End);
    write(' ');
    operator<<(Posfix);
    return *this;
  }

  lang_raw_ostream &exit_block(const char *Posfix = "\n") {
    return exit_block<const char*>(Posfix);
  }

  template<typename CndT, typename PosfixT>
  lang_raw_ostream &if_begin(CndT Cnd, PosfixT Posfix) {
    *this <<"if (" << Cnd << ") ";
    enter_block<PosfixT>(Posfix);
    return *this;
  }

  lang_raw_ostream &if_() {
    *this << "if (";
    return *this;
  }

  template<typename PosfixT>
  lang_raw_ostream &_then(PosfixT Posfix) {
    *this << ") ";
    enter_block<PosfixT>(Posfix);
    return *this;
  }

  lang_raw_ostream &_then(const char* Posfix = "\n") {
    *this << ") ";
    enter_block<const char*>(Posfix);
    return *this;
  }

  template<typename CndT>
  lang_raw_ostream &if_begin(CndT Cnd, const char* Posfix = "\n") {
    return if_begin<CndT, const char*>(Cnd, Posfix);
  }

  template<typename PosfixT>
  lang_raw_ostream &else_begin(PosfixT Posfix) {
    exit_block("else ");
    enter_block<PosfixT>(Posfix);
    return *this;
  }

  lang_raw_ostream &else_begin(const char *Posfix = "\n") {
    return else_begin<const char*>(Posfix);
  }
};

struct CppTraits {
  static const char PrepProcChar = '#';

  static const char *getBlockBegin() {
    static const char *S = "{";
    return S;
  }

  static const char *getBlockEnd() {
    static const char *S = "}";
    return S;
  }
};

typedef lang_raw_ostream<CppTraits> clang_raw_ostream;

struct VerilogTraits {
  static const char PrepProcChar = '`';

  static const char *getBlockBegin() {
    static const char *S = "begin";
    return S;
  }

  static const char *getBlockEnd() {
    static const char *S = "end";
    return S;
  }
};

class vlang_raw_ostream : public lang_raw_ostream<VerilogTraits> {
public:
  explicit vlang_raw_ostream(unsigned Ind = 0)
    : lang_raw_ostream<VerilogTraits>(Ind) {}

  explicit vlang_raw_ostream(raw_ostream &Stream, bool Delete = false,
    unsigned Ind = 0)
    : lang_raw_ostream<VerilogTraits>(Stream, Delete) {}

  vlang_raw_ostream &always_ff_begin(bool PrintReset = true,
                                     const std::string &Clk = "clk",
                                     const std::string &ClkEdge = "posedge",
                                     const std::string &Rst = "rstN",
                                     const std::string &RstEdge = "negedge") {
    *this << "always @(" << ClkEdge << " " << Clk;

    if (PrintReset) *this << ", " << RstEdge << " " << Rst;

    *this << ")";

    enter_block();

    if (!PrintReset) return *this;

    *this << "if (";
    // negative edge reset?
    if (RstEdge == "negedge")
      *this  << "!";
    *this  << Rst << ")";
    enter_block("// reset registers\n");
    return *this;
  }

  vlang_raw_ostream &always_ff_end(bool ExitReset = true) {
    if (ExitReset) exit_block("//else reset\n");
    exit_block("//always @(..)\n\n");
    return *this;
  }

  template<typename CaseT>
  vlang_raw_ostream &switch_begin(CaseT Case) {
    *this << "case (" << Case << ") ";
    enter_block("\n", "");
    Indent -= 2;
    return *this;
  }

  template<typename CaseT>
  vlang_raw_ostream &match_case(CaseT Case) {
    *this << Case << ":";
    enter_block();
    return *this;
  }

  vlang_raw_ostream &switch_end() {
    // Flush the content before change the indent.
    flush();
    Indent += 2;
    exit_block("\n", "endcase");
    return *this;
  }

  vlang_raw_ostream &module_begin() {
    enter_block("\n", "");
    return *this;
  }

  vlang_raw_ostream &module_end() {
    exit_block("\n", "endmodule");
    return *this;
  }
};
}

#endif
