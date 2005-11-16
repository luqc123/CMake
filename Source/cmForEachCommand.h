/*=========================================================================

  Program:   CMake - Cross-Platform Makefile Generator
  Module:    $RCSfile$
  Language:  C++
  Date:      $Date$
  Version:   $Revision$

  Copyright (c) 2002 Kitware, Inc., Insight Consortium.  All rights reserved.
  See Copyright.txt or http://www.cmake.org/HTML/Copyright.html for details.

     This software is distributed WITHOUT ANY WARRANTY; without even 
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR 
     PURPOSE.  See the above copyright notices for more information.

=========================================================================*/
#ifndef cmForEachCommand_h
#define cmForEachCommand_h

#include "cmCommand.h"
#include "cmFunctionBlocker.h"
#include "cmListFileCache.h"

/** \class cmForEachFunctionBlocker
 * \brief subclass of function blocker
 *
 * 
 */
class cmForEachFunctionBlocker : public cmFunctionBlocker
{
public:
  cmForEachFunctionBlocker() {m_Executing = false;}
  virtual ~cmForEachFunctionBlocker() {}
  virtual bool IsFunctionBlocked(const cmListFileFunction& lff,
                                 cmMakefile &mf);
  virtual bool ShouldRemove(const cmListFileFunction& lff, cmMakefile &mf);
  virtual void ScopeEnded(cmMakefile &mf);
  
  std::vector<std::string> m_Args;
  std::vector<cmListFileFunction> m_Functions;
  bool m_Executing;
};

/** \class cmForEachCommand
 * \brief starts an if block
 *
 * cmForEachCommand starts an if block
 */
class cmForEachCommand : public cmCommand
{
public:
  /**
   * This is a virtual constructor for the command.
   */
  virtual cmCommand* Clone() 
    {
    return new cmForEachCommand;
    }

  /**
   * This is called when the command is first encountered in
   * the CMakeLists.txt file.
   */
  virtual bool InitialPass(std::vector<std::string> const& args);

  /**
   * This determines if the command is invoked when in script mode.
   */
  virtual bool IsScriptable() { return true; }

  /**
   * The name of the command as specified in CMakeList.txt.
   */
  virtual const char* GetName() { return "FOREACH";}

  /**
   * Succinct documentation.
   */
  virtual const char* GetTerseDocumentation() 
    {
    return "Evaluate a group of commands for each value in a list.";
    }
  
  /**
   * More documentation.
   */
  virtual const char* GetFullDocumentation()
    {
    return
      "  FOREACH(loop_var arg1 arg2 ...)\n"
      "    COMMAND1(ARGS ...)\n"
      "    COMMAND2(ARGS ...)\n"
      "    ...\n"
      "  ENDFOREACH(loop_var)\n"
      "  FOREACH(loop_var RANGE total)\n"
      "  FOREACH(loop_var RANGE start stop [step])\n"
      "All commands between FOREACH and the matching ENDFOREACH are recorded "
      "without being invoked.  Once the ENDFOREACH is evaluated, the "
      "recorded list of commands is invoked once for each argument listed "
      "in the original FOREACH command.  Before each iteration of the loop "
      "\"${loop_var}\" will be set as a variable with "
      "the current value in the list.\n"
      "Foreach can also iterate over a generated range of numbers. "
      "There are three types of this iteration:\n"
      "* When specifying single number, the range will have elements "
      "0 to \"total\".\n"
      "* When specifying two numbers, the range will have elements from "
      "the first number to the second number.\n"
      "* The third optional number is the increment used to iterate from "
      "the first number to the second number.";
    }
  
  cmTypeMacro(cmForEachCommand, cmCommand);
};


#endif
