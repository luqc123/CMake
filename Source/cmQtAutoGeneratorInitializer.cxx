/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */
#include "cmQtAutoGen.h"
#include "cmQtAutoGeneratorInitializer.h"

#include "cmAlgorithms.h"
#include "cmCustomCommand.h"
#include "cmCustomCommandLines.h"
#include "cmFilePathChecksum.h"
#include "cmGeneratorTarget.h"
#include "cmGlobalGenerator.h"
#include "cmLocalGenerator.h"
#include "cmMakefile.h"
#include "cmOutputConverter.h"
#include "cmPolicies.h"
#include "cmSourceFile.h"
#include "cmSourceGroup.h"
#include "cmState.h"
#include "cmSystemTools.h"
#include "cmTarget.h"
#include "cm_sys_stat.h"
#include "cmake.h"
#include "cmsys/FStream.hxx"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

inline static const char* SafeString(const char* value)
{
  return (value != nullptr) ? value : "";
}

inline static std::string GetSafeProperty(cmGeneratorTarget const* target,
                                          const char* key)
{
  return std::string(SafeString(target->GetProperty(key)));
}

inline static std::string GetSafeProperty(cmSourceFile const* sf,
                                          const char* key)
{
  return std::string(SafeString(sf->GetProperty(key)));
}

inline static bool AutogenMultiConfig(cmGlobalGenerator* globalGen)
{
  return globalGen->IsMultiConfig();
}

static std::string GetAutogenTargetName(cmGeneratorTarget const* target)
{
  std::string autogenTargetName = target->GetName();
  autogenTargetName += "_autogen";
  return autogenTargetName;
}

static std::string GetAutogenTargetFilesDir(cmGeneratorTarget const* target)
{
  cmMakefile* makefile = target->Target->GetMakefile();
  std::string targetDir = makefile->GetCurrentBinaryDirectory();
  targetDir += makefile->GetCMakeInstance()->GetCMakeFilesDirectory();
  targetDir += "/";
  targetDir += GetAutogenTargetName(target);
  targetDir += ".dir";
  return targetDir;
}

static std::string GetAutogenTargetBuildDir(cmGeneratorTarget const* target)
{
  std::string targetDir = GetSafeProperty(target, "AUTOGEN_BUILD_DIR");
  if (targetDir.empty()) {
    cmMakefile* makefile = target->Target->GetMakefile();
    targetDir = makefile->GetCurrentBinaryDirectory();
    targetDir += "/";
    targetDir += GetAutogenTargetName(target);
  }
  return targetDir;
}

std::string cmQtAutoGeneratorInitializer::GetQtMajorVersion(
  cmGeneratorTarget const* target)
{
  cmMakefile* makefile = target->Target->GetMakefile();
  std::string qtMajor = makefile->GetSafeDefinition("QT_VERSION_MAJOR");
  if (qtMajor.empty()) {
    qtMajor = makefile->GetSafeDefinition("Qt5Core_VERSION_MAJOR");
  }
  const char* targetQtVersion =
    target->GetLinkInterfaceDependentStringProperty("QT_MAJOR_VERSION", "");
  if (targetQtVersion != nullptr) {
    qtMajor = targetQtVersion;
  }
  return qtMajor;
}

std::string cmQtAutoGeneratorInitializer::GetQtMinorVersion(
  cmGeneratorTarget const* target, const std::string& qtVersionMajor)
{
  cmMakefile* makefile = target->Target->GetMakefile();
  std::string qtMinor;
  if (qtVersionMajor == "5") {
    qtMinor = makefile->GetSafeDefinition("Qt5Core_VERSION_MINOR");
  }
  if (qtMinor.empty()) {
    qtMinor = makefile->GetSafeDefinition("QT_VERSION_MINOR");
  }

  const char* targetQtVersion =
    target->GetLinkInterfaceDependentStringProperty("QT_MINOR_VERSION", "");
  if (targetQtVersion != nullptr) {
    qtMinor = targetQtVersion;
  }
  return qtMinor;
}

static bool QtVersionGreaterOrEqual(const std::string& major,
                                    const std::string& minor,
                                    unsigned long requestMajor,
                                    unsigned long requestMinor)
{
  unsigned long majorUL(0);
  unsigned long minorUL(0);
  if (cmSystemTools::StringToULong(major.c_str(), &majorUL) &&
      cmSystemTools::StringToULong(minor.c_str(), &minorUL)) {
    return (majorUL > requestMajor) ||
      (majorUL == requestMajor && minorUL >= requestMinor);
  }
  return false;
}

static void GetCompileDefinitionsAndDirectories(
  cmGeneratorTarget const* target, const std::string& config,
  std::string& incs, std::string& defs)
{
  cmLocalGenerator* localGen = target->GetLocalGenerator();
  {
    std::vector<std::string> includeDirs;
    // Get the include dirs for this target, without stripping the implicit
    // include dirs off, see
    // https://gitlab.kitware.com/cmake/cmake/issues/13667
    localGen->GetIncludeDirectories(includeDirs, target, "CXX", config, false);
    incs = cmJoin(includeDirs, ";");
  }
  {
    std::set<std::string> defines;
    localGen->AddCompileDefinitions(defines, target, config, "CXX");
    defs += cmJoin(defines, ";");
  }
}

static std::vector<std::string> GetConfigurations(
  cmMakefile* makefile, std::string* config = nullptr)
{
  std::vector<std::string> configs;
  {
    std::string cfg = makefile->GetConfigurations(configs);
    if (config != nullptr) {
      *config = cfg;
    }
  }
  // Add empty configuration on demand
  if (configs.empty()) {
    configs.push_back("");
  }
  return configs;
}

static std::vector<std::string> GetConfigurationSuffixes(cmMakefile* makefile)
{
  std::vector<std::string> suffixes;
  if (AutogenMultiConfig(makefile->GetGlobalGenerator())) {
    makefile->GetConfigurations(suffixes);
    for (std::string& suffix : suffixes) {
      suffix.insert(0, "_");
    }
  }
  if (suffixes.empty()) {
    suffixes.push_back("");
  }
  return suffixes;
}

static void AddDefinitionEscaped(cmMakefile* makefile, const char* key,
                                 const std::string& value)
{
  makefile->AddDefinition(key,
                          cmOutputConverter::EscapeForCMake(value).c_str());
}

static void AddDefinitionEscaped(cmMakefile* makefile, const char* key,
                                 const std::vector<std::string>& values)
{
  makefile->AddDefinition(
    key, cmOutputConverter::EscapeForCMake(cmJoin(values, ";")).c_str());
}

static void AddDefinitionEscaped(cmMakefile* makefile, const char* key,
                                 const std::set<std::string>& values)
{
  makefile->AddDefinition(
    key, cmOutputConverter::EscapeForCMake(cmJoin(values, ";")).c_str());
}

static bool AddToSourceGroup(cmMakefile* makefile, const std::string& fileName,
                             cmQtAutoGen::GeneratorType genType)
{
  cmSourceGroup* sourceGroup = nullptr;
  // Acquire source group
  {
    std::string property;
    std::string groupName;
    {
      std::array<std::string, 2> props;
      // Use generator specific group name
      switch (genType) {
        case cmQtAutoGen::MOC:
          props[0] = "AUTOMOC_SOURCE_GROUP";
          break;
        case cmQtAutoGen::RCC:
          props[0] = "AUTORCC_SOURCE_GROUP";
          break;
        default:
          props[0] = "AUTOGEN_SOURCE_GROUP";
          break;
      }
      props[1] = "AUTOGEN_SOURCE_GROUP";
      for (std::string& prop : props) {
        const char* propName = makefile->GetState()->GetGlobalProperty(prop);
        if ((propName != nullptr) && (*propName != '\0')) {
          groupName = propName;
          property = std::move(prop);
          break;
        }
      }
    }
    // Generate a source group on demand
    if (!groupName.empty()) {
      {
        const char* delimiter =
          makefile->GetDefinition("SOURCE_GROUP_DELIMITER");
        if (delimiter == nullptr) {
          delimiter = "\\";
        }
        std::vector<std::string> folders =
          cmSystemTools::tokenize(groupName, delimiter);
        sourceGroup = makefile->GetSourceGroup(folders);
        if (sourceGroup == nullptr) {
          makefile->AddSourceGroup(folders);
          sourceGroup = makefile->GetSourceGroup(folders);
        }
      }
      if (sourceGroup == nullptr) {
        std::ostringstream ost;
        ost << cmQtAutoGen::GeneratorNameUpper(genType);
        ost << ": " << property;
        ost << ": Could not find or create the source group ";
        ost << cmQtAutoGen::Quoted(groupName);
        cmSystemTools::Error(ost.str().c_str());
        return false;
      }
    }
  }
  if (sourceGroup != nullptr) {
    sourceGroup->AddGroupFile(fileName);
  }
  return true;
}

static void AddCleanFile(cmMakefile* makefile, const std::string& fileName)
{
  makefile->AppendProperty("ADDITIONAL_MAKE_CLEAN_FILES", fileName.c_str(),
                           false);
}

static void AddGeneratedSource(cmGeneratorTarget* target,
                               const std::string& filename,
                               cmQtAutoGen::GeneratorType genType)
{
  cmMakefile* makefile = target->Target->GetMakefile();
  {
    cmSourceFile* gFile = makefile->GetOrCreateSource(filename, true);
    gFile->SetProperty("GENERATED", "1");
    gFile->SetProperty("SKIP_AUTOGEN", "On");
  }
  target->AddSource(filename);

  AddToSourceGroup(makefile, filename, genType);
}

struct cmQtAutoGenSetup
{
  std::set<std::string> MocSkip;
  std::set<std::string> UicSkip;

  std::map<std::string, std::string> ConfigMocIncludes;
  std::map<std::string, std::string> ConfigMocDefines;
  std::map<std::string, std::string> ConfigUicOptions;
};

static void SetupAcquireSkipFiles(cmQtAutoGenDigest const& digest,
                                  cmQtAutoGenSetup& setup)
{
  // Read skip files from makefile sources
  {
    const std::vector<cmSourceFile*>& allSources =
      digest.Target->Makefile->GetSourceFiles();
    for (cmSourceFile* sf : allSources) {
      // sf->GetExtension() is only valid after sf->GetFullPath() ...
      const std::string& fPath = sf->GetFullPath();
      const cmSystemTools::FileFormat fileType =
        cmSystemTools::GetFileFormat(sf->GetExtension().c_str());
      if (!(fileType == cmSystemTools::CXX_FILE_FORMAT) &&
          !(fileType == cmSystemTools::HEADER_FILE_FORMAT)) {
        continue;
      }
      const bool skipAll = sf->GetPropertyAsBool("SKIP_AUTOGEN");
      const bool mocSkip = digest.MocEnabled &&
        (skipAll || sf->GetPropertyAsBool("SKIP_AUTOMOC"));
      const bool uicSkip = digest.UicEnabled &&
        (skipAll || sf->GetPropertyAsBool("SKIP_AUTOUIC"));
      if (mocSkip || uicSkip) {
        const std::string absFile = cmsys::SystemTools::GetRealPath(fPath);
        if (mocSkip) {
          setup.MocSkip.insert(absFile);
        }
        if (uicSkip) {
          setup.UicSkip.insert(absFile);
        }
      }
    }
  }
}

static void SetupAutoTargetMoc(const cmQtAutoGenDigest& digest,
                               std::string const& config,
                               std::vector<std::string> const& configs,
                               cmQtAutoGenSetup& setup)
{
  cmGeneratorTarget const* target = digest.Target;
  cmLocalGenerator* localGen = target->GetLocalGenerator();
  cmMakefile* makefile = target->Target->GetMakefile();

  AddDefinitionEscaped(makefile, "_moc_skip", setup.MocSkip);
  AddDefinitionEscaped(makefile, "_moc_options",
                       GetSafeProperty(target, "AUTOMOC_MOC_OPTIONS"));
  AddDefinitionEscaped(makefile, "_moc_relaxed_mode",
                       makefile->IsOn("CMAKE_AUTOMOC_RELAXED_MODE") ? "TRUE"
                                                                    : "FALSE");
  AddDefinitionEscaped(makefile, "_moc_macro_names",
                       GetSafeProperty(target, "AUTOMOC_MACRO_NAMES"));
  AddDefinitionEscaped(makefile, "_moc_depend_filters",
                       GetSafeProperty(target, "AUTOMOC_DEPEND_FILTERS"));

  if (QtVersionGreaterOrEqual(digest.QtVersionMajor, digest.QtVersionMinor, 5,
                              8)) {
    AddDefinitionEscaped(
      makefile, "_moc_predefs_cmd",
      makefile->GetSafeDefinition("CMAKE_CXX_COMPILER_PREDEFINES_COMMAND"));
  }
  // Moc includes and compile definitions
  {
    // Default settings
    std::string incs;
    std::string compileDefs;
    GetCompileDefinitionsAndDirectories(target, config, incs, compileDefs);
    AddDefinitionEscaped(makefile, "_moc_incs", incs);
    AddDefinitionEscaped(makefile, "_moc_compile_defs", compileDefs);

    // Configuration specific settings
    for (const std::string& cfg : configs) {
      std::string configIncs;
      std::string configCompileDefs;
      GetCompileDefinitionsAndDirectories(target, cfg, configIncs,
                                          configCompileDefs);
      if (configIncs != incs) {
        setup.ConfigMocIncludes[cfg] = configIncs;
      }
      if (configCompileDefs != compileDefs) {
        setup.ConfigMocDefines[cfg] = configCompileDefs;
      }
    }
  }

  // Moc executable
  {
    std::string mocExec;
    std::string err;

    if (digest.QtVersionMajor == "5") {
      cmGeneratorTarget* tgt = localGen->FindGeneratorTargetToUse("Qt5::moc");
      if (tgt != nullptr) {
        mocExec = SafeString(tgt->ImportedGetLocation(""));
      } else {
        err = "AUTOMOC: Qt5::moc target not found";
      }
    } else if (digest.QtVersionMajor == "4") {
      cmGeneratorTarget* tgt = localGen->FindGeneratorTargetToUse("Qt4::moc");
      if (tgt != nullptr) {
        mocExec = SafeString(tgt->ImportedGetLocation(""));
      } else {
        err = "AUTOMOC: Qt4::moc target not found";
      }
    } else {
      err = "The AUTOMOC feature supports only Qt 4 and Qt 5";
    }

    if (err.empty()) {
      AddDefinitionEscaped(makefile, "_qt_moc_executable", mocExec);
    } else {
      err += " (" + target->GetName() + ")";
      cmSystemTools::Error(err.c_str());
    }
  }
}

static void UicGetOpts(cmGeneratorTarget const* target,
                       const std::string& config, std::string& optString)
{
  std::vector<std::string> opts;
  target->GetAutoUicOptions(opts, config);
  optString = cmJoin(opts, ";");
}

static void SetupAutoTargetUic(const cmQtAutoGenDigest& digest,
                               std::string const& config,
                               std::vector<std::string> const& configs,
                               cmQtAutoGenSetup& setup)
{
  cmGeneratorTarget const* target = digest.Target;
  cmMakefile* makefile = target->Target->GetMakefile();

  AddDefinitionEscaped(makefile, "_uic_skip", setup.UicSkip);

  // Uic search paths
  {
    std::vector<std::string> uicSearchPaths;
    {
      const std::string usp = GetSafeProperty(target, "AUTOUIC_SEARCH_PATHS");
      if (!usp.empty()) {
        cmSystemTools::ExpandListArgument(usp, uicSearchPaths);
        const std::string srcDir = makefile->GetCurrentSourceDirectory();
        for (std::string& path : uicSearchPaths) {
          path = cmSystemTools::CollapseFullPath(path, srcDir);
        }
      }
    }
    AddDefinitionEscaped(makefile, "_uic_search_paths", uicSearchPaths);
  }
  // Uic target options
  {
    // Default settings
    std::string uicOpts;
    UicGetOpts(target, config, uicOpts);
    AddDefinitionEscaped(makefile, "_uic_target_options", uicOpts);

    // Configuration specific settings
    for (const std::string& cfg : configs) {
      std::string configUicOpts;
      UicGetOpts(target, cfg, configUicOpts);
      if (configUicOpts != uicOpts) {
        setup.ConfigUicOptions[cfg] = configUicOpts;
      }
    }
  }
  // Uic files options
  {
    std::vector<std::string> uiFileFiles;
    std::vector<std::string> uiFileOptions;
    {
      const std::string uiExt = "ui";
      const std::vector<cmSourceFile*>& srcFiles = makefile->GetSourceFiles();
      for (cmSourceFile* sf : srcFiles) {
        // sf->GetExtension() is only valid after sf->GetFullPath() ...
        const std::string& fPath = sf->GetFullPath();
        if (sf->GetExtension() == uiExt) {
          // Check if the files has uic options
          std::string uicOpts = GetSafeProperty(sf, "AUTOUIC_OPTIONS");
          if (!uicOpts.empty()) {
            const std::string absFile = cmsys::SystemTools::GetRealPath(fPath);
            // Check if file isn't skipped
            if (setup.UicSkip.count(absFile) == 0) {
              uiFileFiles.push_back(absFile);
              cmSystemTools::ReplaceString(uicOpts, ";", cmQtAutoGen::listSep);
              uiFileOptions.push_back(uicOpts);
            }
          }
        }
      }
    }
    AddDefinitionEscaped(makefile, "_qt_uic_options_files", uiFileFiles);
    AddDefinitionEscaped(makefile, "_qt_uic_options_options", uiFileOptions);
  }

  // Uic executable
  {
    std::string err;
    std::string uicExec;

    cmLocalGenerator* localGen = target->GetLocalGenerator();
    if (digest.QtVersionMajor == "5") {
      cmGeneratorTarget* tgt = localGen->FindGeneratorTargetToUse("Qt5::uic");
      if (tgt != nullptr) {
        uicExec = SafeString(tgt->ImportedGetLocation(""));
      } else {
        // Project does not use Qt5Widgets, but has AUTOUIC ON anyway
      }
    } else if (digest.QtVersionMajor == "4") {
      cmGeneratorTarget* tgt = localGen->FindGeneratorTargetToUse("Qt4::uic");
      if (tgt != nullptr) {
        uicExec = SafeString(tgt->ImportedGetLocation(""));
      } else {
        err = "AUTOUIC: Qt4::uic target not found";
      }
    } else {
      err = "The AUTOUIC feature supports only Qt 4 and Qt 5";
    }

    if (err.empty()) {
      AddDefinitionEscaped(makefile, "_qt_uic_executable", uicExec);
    } else {
      err += " (" + target->GetName() + ")";
      cmSystemTools::Error(err.c_str());
    }
  }
}

static std::string RccGetExecutable(cmGeneratorTarget const* target,
                                    const std::string& qtMajorVersion)
{
  std::string rccExec;
  std::string err;

  cmLocalGenerator* localGen = target->GetLocalGenerator();
  if (qtMajorVersion == "5") {
    cmGeneratorTarget* tgt = localGen->FindGeneratorTargetToUse("Qt5::rcc");
    if (tgt != nullptr) {
      rccExec = SafeString(tgt->ImportedGetLocation(""));
    } else {
      err = "AUTORCC: Qt5::rcc target not found";
    }
  } else if (qtMajorVersion == "4") {
    cmGeneratorTarget* tgt = localGen->FindGeneratorTargetToUse("Qt4::rcc");
    if (tgt != nullptr) {
      rccExec = SafeString(tgt->ImportedGetLocation(""));
    } else {
      err = "AUTORCC: Qt4::rcc target not found";
    }
  } else {
    err = "The AUTORCC feature supports only Qt 4 and Qt 5";
  }

  if (!err.empty()) {
    err += " (" + target->GetName() + ")";
    cmSystemTools::Error(err.c_str());
  }
  return rccExec;
}

static void RccMergeOptions(std::vector<std::string>& opts,
                            const std::vector<std::string>& fileOpts,
                            bool isQt5)
{
  typedef std::vector<std::string>::iterator Iter;
  typedef std::vector<std::string>::const_iterator CIter;
  static const char* valueOptions[4] = { "name", "root", "compress",
                                         "threshold" };

  std::vector<std::string> extraOpts;
  for (CIter fit = fileOpts.begin(), fitEnd = fileOpts.end(); fit != fitEnd;
       ++fit) {
    Iter existIt = std::find(opts.begin(), opts.end(), *fit);
    if (existIt != opts.end()) {
      const char* optName = fit->c_str();
      if (*optName == '-') {
        ++optName;
        if (isQt5 && *optName == '-') {
          ++optName;
        }
      }
      // Test if this is a value option and change the existing value
      if ((optName != fit->c_str()) &&
          std::find_if(cmArrayBegin(valueOptions), cmArrayEnd(valueOptions),
                       cmStrCmp(optName)) != cmArrayEnd(valueOptions)) {
        const Iter existItNext(existIt + 1);
        const CIter fitNext(fit + 1);
        if ((existItNext != opts.end()) && (fitNext != fitEnd)) {
          *existItNext = *fitNext;
          ++fit;
        }
      }
    } else {
      extraOpts.push_back(*fit);
    }
  }
  // Append options
  opts.insert(opts.end(), extraOpts.begin(), extraOpts.end());
}

static void SetupAutoTargetRcc(const cmQtAutoGenDigest& digest)
{
  cmGeneratorTarget const* target = digest.Target;
  cmMakefile* makefile = target->Target->GetMakefile();
  std::vector<std::string> rccFiles;
  std::vector<std::string> rccInputs;
  std::vector<std::string> rccFileFiles;
  std::vector<std::string> rccFileOptions;

  for (const cmQtAutoGenDigestQrc& qrcDigest : digest.Qrcs) {
    const std::string absFile = qrcDigest.QrcFile;
    // Register file
    rccFiles.push_back(absFile);
    // Register (known) resource files
    {
      std::string entriesList = "{";
      if (!qrcDigest.Generated) {
        entriesList += cmJoin(qrcDigest.Resources, cmQtAutoGen::listSep);
      }
      entriesList += "}";
      rccInputs.push_back(entriesList);
    }
    // rcc options for this qrc file
    if (!qrcDigest.Options.empty()) {
      rccFileFiles.push_back(absFile);
      rccFileOptions.push_back(
        cmJoin(qrcDigest.Options, cmQtAutoGen::listSep));
    }
  }

  AddDefinitionEscaped(makefile, "_qt_rcc_executable",
                       RccGetExecutable(target, digest.QtVersionMajor));
  AddDefinitionEscaped(makefile, "_rcc_files", rccFiles);
  AddDefinitionEscaped(makefile, "_rcc_inputs", rccInputs);
  AddDefinitionEscaped(makefile, "_rcc_options_files", rccFileFiles);
  AddDefinitionEscaped(makefile, "_rcc_options_options", rccFileOptions);
}

void cmQtAutoGeneratorInitializer::InitializeAutogenTarget(
  cmQtAutoGenDigest& digest)
{
  cmGeneratorTarget* target = digest.Target;
  cmMakefile* makefile = target->Target->GetMakefile();
  cmLocalGenerator* localGen = target->GetLocalGenerator();
  cmGlobalGenerator* globalGen = localGen->GetGlobalGenerator();

  // Create a custom target for running generators at buildtime
  const bool multiConfig = AutogenMultiConfig(globalGen);
  const std::string autogenTargetName = GetAutogenTargetName(target);
  const std::string autogenBuildDir = GetAutogenTargetBuildDir(target);
  const std::string workingDirectory =
    cmSystemTools::CollapseFullPath("", makefile->GetCurrentBinaryDirectory());
  const std::vector<std::string> suffixes = GetConfigurationSuffixes(makefile);
  std::set<std::string> autogenDepends;
  std::vector<std::string> autogenProvides;

  // Remove build directories on cleanup
  AddCleanFile(makefile, autogenBuildDir);

  // Remove old settings on cleanup
  {
    std::string base = GetAutogenTargetFilesDir(target);
    base += "/AutogenOldSettings";
    for (const std::string& suffix : suffixes) {
      AddCleanFile(makefile, (base + suffix).append(".cmake"));
    }
  }

  // Compose command lines
  cmCustomCommandLines commandLines;
  {
    cmCustomCommandLine currentLine;
    currentLine.push_back(cmSystemTools::GetCMakeCommand());
    currentLine.push_back("-E");
    currentLine.push_back("cmake_autogen");
    currentLine.push_back(GetAutogenTargetFilesDir(target));
    currentLine.push_back("$<CONFIGURATION>");
    commandLines.push_back(currentLine);
  }

  // Compose target comment
  std::string autogenComment;
  {
    std::vector<std::string> toolNames;
    if (digest.MocEnabled) {
      toolNames.push_back("MOC");
    }
    if (digest.UicEnabled) {
      toolNames.push_back("UIC");
    }
    if (digest.RccEnabled) {
      toolNames.push_back("RCC");
    }

    std::string tools = toolNames[0];
    toolNames.erase(toolNames.begin());
    while (toolNames.size() > 1) {
      tools += ", " + toolNames[0];
      toolNames.erase(toolNames.begin());
    }
    if (toolNames.size() == 1) {
      tools += " and " + toolNames[0];
    }
    autogenComment = "Automatic " + tools + " for target " + target->GetName();
  }

  // Add moc compilation to generated files list
  if (digest.MocEnabled) {
    const std::string mocsComp = autogenBuildDir + "/mocs_compilation.cpp";
    AddGeneratedSource(target, mocsComp, cmQtAutoGen::MOC);
    autogenProvides.push_back(mocsComp);
  }

  // Add autogen includes directory to the origin target INCLUDE_DIRECTORIES
  if (digest.MocEnabled || digest.UicEnabled) {
    std::string includeDir = autogenBuildDir + "/include";
    if (multiConfig) {
      includeDir += "_$<CONFIG>";
    }
    target->AddIncludeDirectory(includeDir, true);
  }

  // Extract relevant source files
  {
    const std::string qrcExt = "qrc";
    std::vector<cmSourceFile*> srcFiles;
    target->GetConfigCommonSourceFiles(srcFiles);
    for (cmSourceFile* sf : srcFiles) {
      if (sf->GetPropertyAsBool("SKIP_AUTOGEN")) {
        continue;
      }
      // sf->GetExtension() is only valid after sf->GetFullPath() ...
      const std::string& fPath = sf->GetFullPath();
      const std::string& ext = sf->GetExtension();
      // Register generated files that will be scanned by moc or uic
      if (digest.MocEnabled || digest.UicEnabled) {
        const cmSystemTools::FileFormat fileType =
          cmSystemTools::GetFileFormat(ext.c_str());
        if ((fileType == cmSystemTools::CXX_FILE_FORMAT) ||
            (fileType == cmSystemTools::HEADER_FILE_FORMAT)) {
          const std::string absPath = cmsys::SystemTools::GetRealPath(fPath);
          if ((digest.MocEnabled && !sf->GetPropertyAsBool("SKIP_AUTOMOC")) ||
              (digest.UicEnabled && !sf->GetPropertyAsBool("SKIP_AUTOUIC"))) {
            // Register source
            const bool generated = sf->GetPropertyAsBool("GENERATED");
            if (fileType == cmSystemTools::HEADER_FILE_FORMAT) {
              if (generated) {
                digest.HeadersGenerated.push_back(absPath);
              } else {
                digest.Headers.push_back(absPath);
              }
            } else {
              if (generated) {
                digest.SourcesGenerated.push_back(absPath);
              } else {
                digest.Sources.push_back(absPath);
              }
            }
          }
        }
      }
      // Register rcc enabled files
      if (digest.RccEnabled && (ext == qrcExt) &&
          !sf->GetPropertyAsBool("SKIP_AUTORCC")) {
        // Register qrc file
        {
          cmQtAutoGenDigestQrc qrcDigest;
          qrcDigest.QrcFile = cmsys::SystemTools::GetRealPath(fPath);
          qrcDigest.Generated = sf->GetPropertyAsBool("GENERATED");
          // RCC options
          {
            const std::string opts = GetSafeProperty(sf, "AUTORCC_OPTIONS");
            if (!opts.empty()) {
              cmSystemTools::ExpandListArgument(opts, qrcDigest.Options);
            }
          }
          digest.Qrcs.push_back(std::move(qrcDigest));
        }
      }
    }
    // cmGeneratorTarget::GetConfigCommonSourceFiles computes the target's
    // sources meta data cache. Clear it so that OBJECT library targets that
    // are AUTOGEN initialized after this target get their added
    // mocs_compilation.cpp source acknowledged by this target.
    target->ClearSourcesCache();
  }

  // Process GENERATED sources and headers
  if (!digest.SourcesGenerated.empty() || !digest.HeadersGenerated.empty()) {
    // Check status of policy CMP0071
    bool policyAccept = false;
    bool policyWarn = false;
    const cmPolicies::PolicyStatus CMP0071_status =
      target->Makefile->GetPolicyStatus(cmPolicies::CMP0071);
    switch (CMP0071_status) {
      case cmPolicies::WARN:
        policyWarn = true;
        CM_FALLTHROUGH;
      case cmPolicies::OLD:
        // Ignore GENERATED file
        break;
      case cmPolicies::REQUIRED_IF_USED:
      case cmPolicies::REQUIRED_ALWAYS:
      case cmPolicies::NEW:
        // Process GENERATED file
        policyAccept = true;
        break;
    }

    if (policyAccept) {
      // Accept GENERATED sources
      for (const std::string& absFile : digest.HeadersGenerated) {
        digest.Headers.push_back(absFile);
      }
      for (const std::string& absFile : digest.SourcesGenerated) {
        digest.Sources.push_back(absFile);
      }
    } else if (policyWarn) {
      std::ostringstream ost;
      ost << cmPolicies::GetPolicyWarning(cmPolicies::CMP0071) << "\n";
      ost << "AUTOMOC,AUTOUIC: Ignoring GENERATED source file(s):\n";
      for (const std::string& absFile : digest.HeadersGenerated) {
        ost << "  " << cmQtAutoGen::Quoted(absFile) << "\n";
      }
      for (const std::string& absFile : digest.SourcesGenerated) {
        ost << "  " << cmQtAutoGen::Quoted(absFile) << "\n";
      }
      makefile->IssueMessage(cmake::AUTHOR_WARNING, ost.str());
    }
    // Depend on GENERATED sources even when they are not processed by AUTOGEN
    for (const std::string& absFile : digest.HeadersGenerated) {
      autogenDepends.insert(absFile);
    }
    for (const std::string& absFile : digest.SourcesGenerated) {
      autogenDepends.insert(absFile);
    }
  }
  // Sort headers and sources
  std::sort(digest.Headers.begin(), digest.Headers.end());
  std::sort(digest.Sources.begin(), digest.Sources.end());

  // Process qrc files
  if (!digest.Qrcs.empty()) {
    const bool QtV5 = (digest.QtVersionMajor == "5");
    const cmFilePathChecksum fpathCheckSum(makefile);
    const std::string rcc = RccGetExecutable(target, digest.QtVersionMajor);
    // Target rcc options
    std::vector<std::string> optionsTarget;
    cmSystemTools::ExpandListArgument(
      GetSafeProperty(target, "AUTORCC_OPTIONS"), optionsTarget);

    for (cmQtAutoGenDigestQrc& qrcDigest : digest.Qrcs) {
      // RCC output file name
      {
        std::string rccFile = autogenBuildDir + "/";
        rccFile += fpathCheckSum.getPart(qrcDigest.QrcFile);
        rccFile += "/qrc_";
        rccFile += cmsys::SystemTools::GetFilenameWithoutLastExtension(
          qrcDigest.QrcFile);
        rccFile += ".cpp";

        AddGeneratedSource(target, rccFile, cmQtAutoGen::RCC);
        autogenProvides.push_back(rccFile);
        qrcDigest.RccFile = std::move(rccFile);
      }
      // RCC options
      {
        std::vector<std::string> opts = optionsTarget;
        if (!qrcDigest.Options.empty()) {
          RccMergeOptions(opts, qrcDigest.Options, QtV5);
        }
        qrcDigest.Options = std::move(opts);
      }
      // GENERATED or not
      if (qrcDigest.Generated) {
        // Add GENERATED qrc file to the dependencies
        autogenDepends.insert(qrcDigest.QrcFile);
      } else {
        // Add the resource files to the dependencies
        {
          std::string error;
          if (cmQtAutoGen::RccListInputs(digest.QtVersionMajor, rcc,
                                         qrcDigest.QrcFile,
                                         qrcDigest.Resources, &error)) {
            autogenDepends.insert(qrcDigest.Resources.begin(),
                                  qrcDigest.Resources.end());
          } else {
            cmSystemTools::Error(error.c_str());
          }
        }
        // Run cmake again when .qrc file changes
        makefile->AddCMakeDependFile(qrcDigest.QrcFile);
      }
    }
  }

  // Add user defined autogen target dependencies
  {
    const std::string deps = GetSafeProperty(target, "AUTOGEN_TARGET_DEPENDS");
    if (!deps.empty()) {
      std::vector<std::string> extraDeps;
      cmSystemTools::ExpandListArgument(deps, extraDeps);
      autogenDepends.insert(extraDeps.begin(), extraDeps.end());
    }
  }
  // Add utility target dependencies to the autogen target dependencies
  {
    const std::set<std::string>& utils = target->Target->GetUtilities();
    for (const std::string& targetName : utils) {
      if (makefile->FindTargetToUse(targetName) != nullptr) {
        autogenDepends.insert(targetName);
      }
    }
  }
  // Add link library target dependencies to the autogen target dependencies
  {
    const auto& libVec = target->Target->GetOriginalLinkLibraries();
    for (const auto& item : libVec) {
      if (makefile->FindTargetToUse(item.first) != nullptr) {
        autogenDepends.insert(item.first);
      }
    }
  }

  // Convert std::set to std::vector
  const std::vector<std::string> depends(autogenDepends.begin(),
                                         autogenDepends.end());
  autogenDepends.clear();

  // Use PRE_BUILD on demand
  bool usePRE_BUILD = false;
  if (globalGen->GetName().find("Visual Studio") != std::string::npos) {
    // Under VS use a PRE_BUILD event instead of a separate target to
    // reduce the number of targets loaded into the IDE.
    // This also works around a VS 11 bug that may skip updating the target:
    //  https://connect.microsoft.com/VisualStudio/feedback/details/769495
    usePRE_BUILD = true;
  }
  // Disable PRE_BUILD in some cases
  if (usePRE_BUILD) {
    // - Cannot use PRE_BUILD with GENERATED qrc files because the
    // resource files themselves may not be sources within the target
    // so VS may not know the target needs to re-build at all.
    for (cmQtAutoGenDigestQrc& qrcDigest : digest.Qrcs) {
      if (qrcDigest.Generated) {
        usePRE_BUILD = false;
        break;
      }
    }
  }
  // Create the autogen target/command
  if (usePRE_BUILD) {
    // Add the pre-build command directly to bypass the OBJECT_LIBRARY
    // rejection in cmMakefile::AddCustomCommandToTarget because we know
    // PRE_BUILD will work for an OBJECT_LIBRARY in this specific case.
    const std::vector<std::string> no_output;
    cmCustomCommand cc(makefile, no_output, autogenProvides, depends,
                       commandLines, autogenComment.c_str(),
                       workingDirectory.c_str());
    cc.SetEscapeOldStyle(false);
    cc.SetEscapeAllowMakeVars(true);
    target->Target->AddPreBuildCommand(cc);
  } else {
    cmTarget* autogenTarget = makefile->AddUtilityCommand(
      autogenTargetName, true, workingDirectory.c_str(),
      /*byproducts=*/autogenProvides, depends, commandLines, false,
      autogenComment.c_str());

    localGen->AddGeneratorTarget(
      new cmGeneratorTarget(autogenTarget, localGen));

    // Set autogen target FOLDER
    {
      const char* autogenFolder =
        makefile->GetState()->GetGlobalProperty("AUTOMOC_TARGETS_FOLDER");
      if (autogenFolder == nullptr) {
        autogenFolder =
          makefile->GetState()->GetGlobalProperty("AUTOGEN_TARGETS_FOLDER");
      }
      // Inherit FOLDER property from target (#13688)
      if (autogenFolder == nullptr) {
        autogenFolder = SafeString(target->Target->GetProperty("FOLDER"));
      }
      if ((autogenFolder != nullptr) && (*autogenFolder != '\0')) {
        autogenTarget->SetProperty("FOLDER", autogenFolder);
      }
    }

    // Add autogen target to the origin target dependencies
    target->Target->AddUtility(autogenTargetName);
  }
}

void cmQtAutoGeneratorInitializer::SetupAutoGenerateTarget(
  const cmQtAutoGenDigest& digest)
{
  cmGeneratorTarget const* target = digest.Target;
  cmMakefile* makefile = target->Target->GetMakefile();

  // forget the variables added here afterwards again:
  cmMakefile::ScopePushPop varScope(makefile);
  static_cast<void>(varScope);

  // Get configurations
  std::string config;
  const std::vector<std::string> configs(GetConfigurations(makefile, &config));

  // Configuration suffixes
  std::map<std::string, std::string> configSuffix;
  if (AutogenMultiConfig(target->GetGlobalGenerator())) {
    for (const std::string& cfg : configs) {
      configSuffix[cfg] = "_" + cfg;
    }
  }

  // Configurations settings buffers
  cmQtAutoGenSetup setup;

  // Basic setup
  AddDefinitionEscaped(makefile, "_build_dir",
                       GetAutogenTargetBuildDir(target));
  AddDefinitionEscaped(makefile, "_qt_version_major", digest.QtVersionMajor);
  AddDefinitionEscaped(makefile, "_qt_version_minor", digest.QtVersionMinor);
  AddDefinitionEscaped(makefile, "_sources", digest.Sources);
  AddDefinitionEscaped(makefile, "_headers", digest.Headers);
  {
    if (digest.MocEnabled || digest.UicEnabled) {
      SetupAcquireSkipFiles(digest, setup);
      if (digest.MocEnabled) {
        SetupAutoTargetMoc(digest, config, configs, setup);
      }
      if (digest.UicEnabled) {
        SetupAutoTargetUic(digest, config, configs, setup);
      }
    }
    if (digest.RccEnabled) {
      SetupAutoTargetRcc(digest);
    }
  }

  // Generate info file
  std::string infoFile = GetAutogenTargetFilesDir(target);
  infoFile += "/AutogenInfo.cmake";
  {
    std::string inf = cmSystemTools::GetCMakeRoot();
    inf += "/Modules/AutogenInfo.cmake.in";
    makefile->ConfigureFile(inf.c_str(), infoFile.c_str(), false, true, false);
  }

  // Append custom definitions to info file on demand
  if (!configSuffix.empty() || !setup.ConfigMocDefines.empty() ||
      !setup.ConfigMocIncludes.empty() || !setup.ConfigUicOptions.empty()) {

    // Ensure we have write permission in case .in was read-only.
    mode_t perm = 0;
#if defined(_WIN32) && !defined(__CYGWIN__)
    mode_t mode_write = S_IWRITE;
#else
    mode_t mode_write = S_IWUSR;
#endif
    cmSystemTools::GetPermissions(infoFile, perm);
    if (!(perm & mode_write)) {
      cmSystemTools::SetPermissions(infoFile, perm | mode_write);
    }

    // Open and write file
    cmsys::ofstream ofs(infoFile.c_str(), std::ios::app);
    if (ofs) {
      ofs << "# Configuration specific options\n";
      for (const auto& item : configSuffix) {
        ofs << "set(AM_CONFIG_SUFFIX_" << item.first << " "
            << cmOutputConverter::EscapeForCMake(item.second) << ")\n";
      }
      for (const auto& item : setup.ConfigMocDefines) {
        ofs << "set(AM_MOC_DEFINITIONS_" << item.first << " "
            << cmOutputConverter::EscapeForCMake(item.second) << ")\n";
      }
      for (const auto& item : setup.ConfigMocIncludes) {
        ofs << "set(AM_MOC_INCLUDES_" << item.first << " "
            << cmOutputConverter::EscapeForCMake(item.second) << ")\n";
      }
      for (const auto& item : setup.ConfigUicOptions) {
        ofs << "set(AM_UIC_TARGET_OPTIONS_" << item.first << " "
            << cmOutputConverter::EscapeForCMake(item.second) << ")\n";
      }
    } else {
      // File open error
      std::string error = "Internal CMake error when trying to open file: ";
      error += cmQtAutoGen::Quoted(infoFile);
      error += " for writing.";
      cmSystemTools::Error(error.c_str());
    }
  }
}
