@echo off

rem file      : bootstrap-msvc.bat
rem copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
rem license   : MIT; see accompanying LICENSE file

setlocal EnableDelayedExpansion
goto start

:usage
echo.
echo Usage: %0 [/?] [cl-exe [cl-option...]]
echo.
echo Normally this batch file is executed from one of the Visual Studio
echo command prompts. It assume that the VC compiler can be executed as
echo just cl.exe and that all the relevant environment variables (INCLUDE,
echo LIB) are set.
echo.
echo The batch file expects to find the libbutl\ or libbutl-*\ directory
echo either in the current directory (build2 root) or one level up.
echo.
echo Note that is any cl-option arguments are specified, then they must be
echo preceded by the VC compiler executable (use cl.exe as the default).
echo For example:
echo.
echo %0 cl.exe /nologo
echo.
goto end

rem Clean up .obj files from all the directories passed as arguments.
rem
:clean_obj
  for %%d in (%*) do (
    if exist %%d\*.obj del %%d\*.obj
  )
goto :eof

:start

if "_%1_" == "_/?_" goto usage

rem See if there is libbutl or libbutl-* in the current directory and one
rem directory up. Note that globbing returns paths in alphabetic order.
rem
if exist libbutl\ (
  set "libbutl=libbutl"
) else (
  for /D %%d in (libbutl-*) do set "libbutl=%%d"
)

if "_%libbutl%_" == "__" (
  if exist ..\libbutl\ (
      set "libbutl=..\libbutl"
  ) else (
    for /D %%d in (..\libbutl-*) do set "libbutl=%%d"
  )
)

if "_%libbutl%_" == "__" (
  echo error: unable to find libbutl, run %0 /? for details
  goto error
)

rem All the source directories.
rem
set "src=build2"
set "src=%src% build2\config"
set "src=%src% build2\dist"
set "src=%src% build2\bin"
set "src=%src% build2\c"
set "src=%src% build2\cc"
set "src=%src% build2\cxx"
set "src=%src% build2\cli"
set "src=%src% build2\test"
set "src=%src% build2\install"
set "src=%src% %libbutl%\butl"

rem Get the compiler executable.
rem
if "_%1_" == "__" (
  set "cxx=cl.exe"
) else (
  set "cxx=%1"
)

rem Get the compile options.
rem
set "ops=/EHsc /MT /MP"
:ops_next
shift
if "_%1_" == "__" (
  goto ops_done
) else (
  set "ops=%ops% %1"
  goto ops_next
)
:ops_done

rem First clean up any stale .obj files we might have laying around.
rem
call :clean_obj %src%

rem Compile.
rem
rem VC dumps .obj files in the current directory not caring if the names
rem clash. And boy do they clash.
rem
set "obj="
set "cwd=%CD%"
for %%d in (%src%) do (
  echo.
  echo compiling in %%d\
  echo.
  cd %%d
  echo %cxx% /I%cwd%\%libbutl% /I%cwd% /DBUILD2_HOST_TRIPLET=\"i686-microsoft-win32-msvc\" %ops% /c /TP *.cxx
       %cxx% /I%cwd%\%libbutl% /I%cwd% /DBUILD2_HOST_TRIPLET=\"i686-microsoft-win32-msvc\" %ops% /c /TP *.cxx
  if errorlevel 1 (
    cd %cwd%
    goto error
  )
  cd %cwd%
  set "obj=!obj! %%d\*.obj"
)

rem Link.
rem
echo.
echo %cxx% %ops% /Fe: build2\b-boot.exe %obj% shell32.lib
     %cxx% %ops% /Fe: build2\b-boot.exe %obj% shell32.lib
if errorlevel 1 goto error

rem Clean up .obj.
rem
call :clean_obj %src%
goto end

:error
endlocal
exit /b 1

:end
endlocal