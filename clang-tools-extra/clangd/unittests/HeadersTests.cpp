//===-- HeadersTests.cpp - Include headers unit tests -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Headers.h"

#include "Compiler.h"
#include "Matchers.h"
#include "TestFS.h"
#include "TestTU.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/Inclusions/HeaderIncludes.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Path.h"
#include "llvm/Testing/Support/Error.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <optional>

namespace clang {
namespace clangd {
namespace {

using ::testing::AllOf;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::UnorderedElementsAre;

class HeadersTest : public ::testing::Test {
public:
  HeadersTest() {
    CDB.ExtraClangFlags = {SearchDirArg.c_str()};
    FS.Files[MainFile] = "";
    // Make sure directory sub/ exists.
    FS.Files[testPath("sub/EMPTY")] = "";
  }

private:
  std::unique_ptr<CompilerInstance> setupClang() {
    auto Cmd = CDB.getCompileCommand(MainFile);
    assert(static_cast<bool>(Cmd));

    ParseInputs PI;
    PI.CompileCommand = *Cmd;
    PI.TFS = &FS;
    auto CI = buildCompilerInvocation(PI, IgnoreDiags);
    EXPECT_TRUE(static_cast<bool>(CI));
    // The diagnostic options must be set before creating a CompilerInstance.
    CI->getDiagnosticOpts().IgnoreWarnings = true;
    auto VFS = PI.TFS->view(Cmd->Directory);
    auto Clang = prepareCompilerInstance(
        std::move(CI), /*Preamble=*/nullptr,
        llvm::MemoryBuffer::getMemBuffer(FS.Files[MainFile], MainFile),
        std::move(VFS), IgnoreDiags);

    EXPECT_FALSE(Clang->getFrontendOpts().Inputs.empty());
    return Clang;
  }

protected:
  IncludeStructure::HeaderID getID(StringRef Filename,
                                   IncludeStructure &Includes) {
    auto &SM = Clang->getSourceManager();
    auto Entry = SM.getFileManager().getFileRef(Filename);
    EXPECT_THAT_EXPECTED(Entry, llvm::Succeeded());
    return Includes.getOrCreateID(*Entry);
  }

  IncludeStructure collectIncludes() {
    Clang = setupClang();
    PreprocessOnlyAction Action;
    EXPECT_TRUE(
        Action.BeginSourceFile(*Clang, Clang->getFrontendOpts().Inputs[0]));
    IncludeStructure Includes;
    Includes.collect(*Clang);
    EXPECT_FALSE(Action.Execute());
    Action.EndSourceFile();
    return Includes;
  }

  // Calculates the include path, or returns "" on error or header should not be
  // inserted.
  std::string calculate(PathRef Original, PathRef Preferred = "",
                        const std::vector<Inclusion> &Inclusions = {}) {
    Clang = setupClang();
    PreprocessOnlyAction Action;
    EXPECT_TRUE(
        Action.BeginSourceFile(*Clang, Clang->getFrontendOpts().Inputs[0]));

    if (Preferred.empty())
      Preferred = Original;
    auto ToHeaderFile = [](llvm::StringRef Header) {
      return HeaderFile{std::string(Header),
                        /*Verbatim=*/!llvm::sys::path::is_absolute(Header)};
    };

    IncludeInserter Inserter(MainFile, /*Code=*/"", format::getLLVMStyle(),
                             CDB.getCompileCommand(MainFile)->Directory,
                             &Clang->getPreprocessor().getHeaderSearchInfo(),
                             QuotedHeaders, AngledHeaders);
    for (const auto &Inc : Inclusions)
      Inserter.addExisting(Inc);
    auto Inserted = ToHeaderFile(Preferred);
    if (!Inserter.shouldInsertInclude(Original, Inserted))
      return "";
    auto Path = Inserter.calculateIncludePath(Inserted, MainFile);
    Action.EndSourceFile();
    return Path.value_or("");
  }

  std::optional<TextEdit> insert(llvm::StringRef VerbatimHeader,
                                 tooling::IncludeDirective Directive) {
    Clang = setupClang();
    PreprocessOnlyAction Action;
    EXPECT_TRUE(
        Action.BeginSourceFile(*Clang, Clang->getFrontendOpts().Inputs[0]));

    IncludeInserter Inserter(MainFile, /*Code=*/"", format::getLLVMStyle(),
                             CDB.getCompileCommand(MainFile)->Directory,
                             &Clang->getPreprocessor().getHeaderSearchInfo(),
                             QuotedHeaders, AngledHeaders);
    auto Edit = Inserter.insert(VerbatimHeader, Directive);
    Action.EndSourceFile();
    return Edit;
  }

  MockFS FS;
  MockCompilationDatabase CDB;
  std::string MainFile = testPath("main.cpp");
  std::string Subdir = testPath("sub");
  std::string SearchDirArg = (llvm::Twine("-I") + Subdir).str();
  IgnoringDiagConsumer IgnoreDiags;
  std::vector<std::function<bool(llvm::StringRef)>> QuotedHeaders;
  std::vector<std::function<bool(llvm::StringRef)>> AngledHeaders;
  std::unique_ptr<CompilerInstance> Clang;
};

MATCHER_P(written, Name, "") { return arg.Written == Name; }
MATCHER_P(resolved, Name, "") { return arg.Resolved == Name; }
MATCHER_P(includeLine, N, "") { return arg.HashLine == N; }
MATCHER_P(directive, D, "") { return arg.Directive == D; }

MATCHER_P2(Distance, File, D, "") {
  if (arg.getFirst() != File)
    *result_listener << "file =" << static_cast<unsigned>(arg.getFirst());
  if (arg.getSecond() != D)
    *result_listener << "distance =" << arg.getSecond();
  return arg.getFirst() == File && arg.getSecond() == D;
}

TEST_F(HeadersTest, CollectRewrittenAndResolved) {
  FS.Files[MainFile] = R"cpp(
#include "sub/bar.h" // not shortest
)cpp";
  std::string BarHeader = testPath("sub/bar.h");
  FS.Files[BarHeader] = "";

  auto Includes = collectIncludes();
  EXPECT_THAT(Includes.MainFileIncludes,
              UnorderedElementsAre(
                  AllOf(written("\"sub/bar.h\""), resolved(BarHeader))));
  EXPECT_THAT(Includes.includeDepth(getID(MainFile, Includes)),
              UnorderedElementsAre(Distance(getID(MainFile, Includes), 0u),
                                   Distance(getID(BarHeader, Includes), 1u)));
}

TEST_F(HeadersTest, OnlyCollectInclusionsInMain) {
  std::string BazHeader = testPath("sub/baz.h");
  FS.Files[BazHeader] = "";
  std::string BarHeader = testPath("sub/bar.h");
  FS.Files[BarHeader] = R"cpp(
#include "baz.h"
)cpp";
  FS.Files[MainFile] = R"cpp(
#include "bar.h"
)cpp";
  auto Includes = collectIncludes();
  EXPECT_THAT(
      Includes.MainFileIncludes,
      UnorderedElementsAre(AllOf(written("\"bar.h\""), resolved(BarHeader))));
  EXPECT_THAT(Includes.includeDepth(getID(MainFile, Includes)),
              UnorderedElementsAre(Distance(getID(MainFile, Includes), 0u),
                                   Distance(getID(BarHeader, Includes), 1u),
                                   Distance(getID(BazHeader, Includes), 2u)));
  // includeDepth() also works for non-main files.
  EXPECT_THAT(Includes.includeDepth(getID(BarHeader, Includes)),
              UnorderedElementsAre(Distance(getID(BarHeader, Includes), 0u),
                                   Distance(getID(BazHeader, Includes), 1u)));
}

TEST_F(HeadersTest, CacheBySpellingIsBuiltForMainInclusions) {
  std::string FooHeader = testPath("foo.h");
  FS.Files[FooHeader] = R"cpp(
  void foo();
)cpp";
  std::string BarHeader = testPath("bar.h");
  FS.Files[BarHeader] = R"cpp(
  void bar();
)cpp";
  std::string BazHeader = testPath("baz.h");
  FS.Files[BazHeader] = R"cpp(
  void baz();
)cpp";
  FS.Files[MainFile] = R"cpp(
#include "foo.h"
#include "bar.h"
#include "baz.h"
)cpp";
  auto Includes = collectIncludes();
  EXPECT_THAT(Includes.MainFileIncludes,
              UnorderedElementsAre(written("\"foo.h\""), written("\"bar.h\""),
                                   written("\"baz.h\"")));
  EXPECT_THAT(Includes.mainFileIncludesWithSpelling("\"foo.h\""),
              UnorderedElementsAre(&Includes.MainFileIncludes[0]));
  EXPECT_THAT(Includes.mainFileIncludesWithSpelling("\"bar.h\""),
              UnorderedElementsAre(&Includes.MainFileIncludes[1]));
  EXPECT_THAT(Includes.mainFileIncludesWithSpelling("\"baz.h\""),
              UnorderedElementsAre(&Includes.MainFileIncludes[2]));
}

TEST_F(HeadersTest, PreambleIncludesPresentOnce) {
  // We use TestTU here, to ensure we use the preamble replay logic.
  // We're testing that the logic doesn't crash, and doesn't result in duplicate
  // includes. (We'd test more directly, but it's pretty well encapsulated!)
  auto TU = TestTU::withCode(R"cpp(
    #include "a.h"

    #include "a.h"
    void foo();
    #include "a.h"
  )cpp");
  TU.HeaderFilename = "a.h"; // suppress "not found".
  EXPECT_THAT(TU.build().getIncludeStructure().MainFileIncludes,
              ElementsAre(includeLine(1), includeLine(3), includeLine(5)));
}

TEST_F(HeadersTest, UnResolvedInclusion) {
  FS.Files[MainFile] = R"cpp(
#include "foo.h"
)cpp";

  EXPECT_THAT(collectIncludes().MainFileIncludes,
              UnorderedElementsAre(AllOf(written("\"foo.h\""), resolved(""))));
  EXPECT_THAT(collectIncludes().IncludeChildren, IsEmpty());
}

TEST_F(HeadersTest, IncludedFilesGraph) {
  FS.Files[MainFile] = R"cpp(
#include "bar.h"
#include "foo.h"
)cpp";
  std::string BarHeader = testPath("bar.h");
  FS.Files[BarHeader] = "";
  std::string FooHeader = testPath("foo.h");
  FS.Files[FooHeader] = R"cpp(
#include "bar.h"
#include "baz.h"
)cpp";
  std::string BazHeader = testPath("baz.h");
  FS.Files[BazHeader] = "";

  auto Includes = collectIncludes();
  llvm::DenseMap<IncludeStructure::HeaderID,
                 SmallVector<IncludeStructure::HeaderID>>
      Expected = {{getID(MainFile, Includes),
                   {getID(BarHeader, Includes), getID(FooHeader, Includes)}},
                  {getID(FooHeader, Includes),
                   {getID(BarHeader, Includes), getID(BazHeader, Includes)}}};
  EXPECT_EQ(Includes.IncludeChildren, Expected);
}

TEST_F(HeadersTest, IncludeDirective) {
  FS.Files[MainFile] = R"cpp(
#include "foo.h"
#import "foo.h"
#include_next "foo.h"
)cpp";

  // ms-compatibility changes meaning of #import, make sure it is turned off.
  CDB.ExtraClangFlags.push_back("-fno-ms-compatibility");
  EXPECT_THAT(collectIncludes().MainFileIncludes,
              UnorderedElementsAre(directive(tok::pp_include),
                                   directive(tok::pp_import),
                                   directive(tok::pp_include_next)));
}

TEST_F(HeadersTest, SearchPath) {
  FS.Files["foo/bar.h"] = "x";
  FS.Files["foo/bar/baz.h"] = "y";
  CDB.ExtraClangFlags.push_back("-Ifoo/bar");
  CDB.ExtraClangFlags.push_back("-Ifoo/bar/..");
  EXPECT_THAT(collectIncludes().SearchPathsCanonical,
              ElementsAre(Subdir, testPath("foo/bar"), testPath("foo")));
}

TEST_F(HeadersTest, InsertInclude) {
  std::string Path = testPath("sub/bar.h");
  FS.Files[Path] = "";
  EXPECT_EQ(calculate(Path), "\"bar.h\"");

  AngledHeaders.push_back([](auto Path) { return true; });
  EXPECT_EQ(calculate(Path), "<bar.h>");
}

TEST_F(HeadersTest, DoNotInsertIfInSameFile) {
  MainFile = testPath("main.h");
  EXPECT_EQ(calculate(MainFile), "");
}

TEST_F(HeadersTest, DoNotInsertOffIncludePath) {
  MainFile = testPath("sub/main.cpp");
  EXPECT_EQ(calculate(testPath("sub2/main.cpp")), "");
}

TEST_F(HeadersTest, ShortenIncludesInSearchPath) {
  std::string BarHeader = testPath("sub/bar.h");
  EXPECT_EQ(calculate(BarHeader), "\"bar.h\"");

  SearchDirArg = (llvm::Twine("-I") + Subdir + "/..").str();
  CDB.ExtraClangFlags = {SearchDirArg.c_str()};
  BarHeader = testPath("sub/bar.h");
  EXPECT_EQ(calculate(BarHeader), "\"sub/bar.h\"");
}

TEST_F(HeadersTest, ShortenIncludesInSearchPathBracketed) {
  AngledHeaders.push_back([](auto Path) { return true; });
  std::string BarHeader = testPath("sub/bar.h");
  EXPECT_EQ(calculate(BarHeader), "<bar.h>");

  SearchDirArg = (llvm::Twine("-I") + Subdir + "/..").str();
  CDB.ExtraClangFlags = {SearchDirArg.c_str()};
  BarHeader = testPath("sub/bar.h");
  EXPECT_EQ(calculate(BarHeader), "<sub/bar.h>");
}

TEST_F(HeadersTest, ShortenedIncludeNotInSearchPath) {
  std::string BarHeader =
      llvm::sys::path::convert_to_slash(testPath("sub-2/bar.h"));
  EXPECT_EQ(calculate(BarHeader, ""), "\"sub-2/bar.h\"");
}

TEST_F(HeadersTest, PreferredHeader) {
  std::string BarHeader = testPath("sub/bar.h");
  EXPECT_EQ(calculate(BarHeader, "<bar>"), "<bar>");

  std::string BazHeader = testPath("sub/baz.h");
  EXPECT_EQ(calculate(BarHeader, BazHeader), "\"baz.h\"");

  AngledHeaders.push_back([](auto Path) { return true; });
  std::string BiffHeader = testPath("sub/biff.h");
  EXPECT_EQ(calculate(BarHeader, BiffHeader), "<biff.h>");
}

TEST_F(HeadersTest, DontInsertDuplicatePreferred) {
  Inclusion Inc;
  Inc.Written = "\"bar.h\"";
  Inc.Resolved = "";
  EXPECT_EQ(calculate(testPath("sub/bar.h"), "\"bar.h\"", {Inc}), "");
  EXPECT_EQ(calculate("\"x.h\"", "\"bar.h\"", {Inc}), "");
}

TEST_F(HeadersTest, DontInsertDuplicateResolved) {
  Inclusion Inc;
  Inc.Written = "fake-bar.h";
  Inc.Resolved = testPath("sub/bar.h");
  EXPECT_EQ(calculate(Inc.Resolved, "", {Inc}), "");
  // Do not insert preferred.
  EXPECT_EQ(calculate(Inc.Resolved, "\"BAR.h\"", {Inc}), "");
}

TEST_F(HeadersTest, PreferInserted) {
  auto Edit = insert("<y>", tooling::IncludeDirective::Include);
  ASSERT_TRUE(Edit);
  EXPECT_EQ(Edit->newText, "#include <y>\n");

  Edit = insert("\"header.h\"", tooling::IncludeDirective::Import);
  ASSERT_TRUE(Edit);
  EXPECT_EQ(Edit->newText, "#import \"header.h\"\n");
}

TEST(Headers, NoHeaderSearchInfo) {
  std::string MainFile = testPath("main.cpp");
  IncludeInserter Inserter(MainFile, /*Code=*/"", format::getLLVMStyle(),
                           /*BuildDir=*/"", /*HeaderSearchInfo=*/nullptr,
                           /*QuotedHeaders=*/{}, /*AngledHeaders=*/{});

  auto HeaderPath = testPath("sub/bar.h");
  auto Inserting = HeaderFile{HeaderPath, /*Verbatim=*/false};
  auto Verbatim = HeaderFile{"<x>", /*Verbatim=*/true};

  EXPECT_EQ(Inserter.calculateIncludePath(Inserting, MainFile),
            std::string("\"sub/bar.h\""));
  EXPECT_EQ(Inserter.shouldInsertInclude(HeaderPath, Inserting), false);

  EXPECT_EQ(Inserter.calculateIncludePath(Verbatim, MainFile),
            std::string("<x>"));
  EXPECT_EQ(Inserter.shouldInsertInclude(HeaderPath, Verbatim), true);

  EXPECT_EQ(Inserter.calculateIncludePath(Inserting, "sub2/main2.cpp"),
            std::nullopt);
}

TEST_F(HeadersTest, PresumedLocations) {
  std::string HeaderFile = "__preamble_patch__.h";

  // Line map inclusion back to main file.
  std::string HeaderContents =
      llvm::formatv("#line 0 \"{0}\"", llvm::sys::path::filename(MainFile));
  HeaderContents += R"cpp(
#line 3
#include <a.h>)cpp";
  FS.Files[HeaderFile] = HeaderContents;

  // Including through non-builtin file has no effects.
  FS.Files[MainFile] = "#include \"__preamble_patch__.h\"\n\n";
  EXPECT_THAT(collectIncludes().MainFileIncludes,
              Not(Contains(written("<a.h>"))));

  // Now include through built-in file.
  CDB.ExtraClangFlags = {"-include", testPath(HeaderFile)};
  EXPECT_THAT(collectIncludes().MainFileIncludes,
              Contains(AllOf(includeLine(2), written("<a.h>"))));
}

} // namespace
} // namespace clangd
} // namespace clang
