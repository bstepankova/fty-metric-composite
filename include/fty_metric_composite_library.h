/*  =========================================================================
    fty-metric-composite - generated layer of public API

    Copyright (C) 2014 - 2017 Eaton                                        
                                                                           
    This program is free software; you can redistribute it and/or modify   
    it under the terms of the GNU General Public License as published by   
    the Free Software Foundation; either version 2 of the License, or      
    (at your option) any later version.                                    
                                                                           
    This program is distributed in the hope that it will be useful,        
    but WITHOUT ANY WARRANTY; without even the implied warranty of         
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          
    GNU General Public License for more details.                           
                                                                           
    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.            

################################################################################
#  THIS FILE IS 100% GENERATED BY ZPROJECT; DO NOT EDIT EXCEPT EXPERIMENTALLY  #
#  Read the zproject/README.md for information about making permanent changes. #
################################################################################
    =========================================================================
*/

#ifndef FTY_METRIC_COMPOSITE_LIBRARY_H_INCLUDED
#define FTY_METRIC_COMPOSITE_LIBRARY_H_INCLUDED

//  Set up environment for the application

//  External dependencies
#include <czmq.h>
#include <malamute.h>
#include <lua.hpp>
#include <ftyproto.h>
#include <cxxtools/allocator.h>

//  FTY_METRIC_COMPOSITE version macros for compile-time API detection
#define FTY_METRIC_COMPOSITE_VERSION_MAJOR 1
#define FTY_METRIC_COMPOSITE_VERSION_MINOR 0
#define FTY_METRIC_COMPOSITE_VERSION_PATCH 0

#define FTY_METRIC_COMPOSITE_MAKE_VERSION(major, minor, patch) \
    ((major) * 10000 + (minor) * 100 + (patch))
#define FTY_METRIC_COMPOSITE_VERSION \
    FTY_METRIC_COMPOSITE_MAKE_VERSION(FTY_METRIC_COMPOSITE_VERSION_MAJOR, FTY_METRIC_COMPOSITE_VERSION_MINOR, FTY_METRIC_COMPOSITE_VERSION_PATCH)

#if defined (__WINDOWS__)
#   if defined FTY_METRIC_COMPOSITE_STATIC
#       define FTY_METRIC_COMPOSITE_EXPORT
#   elif defined FTY_METRIC_COMPOSITE_INTERNAL_BUILD
#       if defined DLL_EXPORT
#           define FTY_METRIC_COMPOSITE_EXPORT __declspec(dllexport)
#       else
#           define FTY_METRIC_COMPOSITE_EXPORT
#       endif
#   elif defined FTY_METRIC_COMPOSITE_EXPORTS
#       define FTY_METRIC_COMPOSITE_EXPORT __declspec(dllexport)
#   else
#       define FTY_METRIC_COMPOSITE_EXPORT __declspec(dllimport)
#   endif
#   define FTY_METRIC_COMPOSITE_PRIVATE
#else
#   define FTY_METRIC_COMPOSITE_EXPORT
#   if (defined __GNUC__ && __GNUC__ >= 4) || defined __INTEL_COMPILER
#       define FTY_METRIC_COMPOSITE_PRIVATE __attribute__ ((visibility ("hidden")))
#   else
#       define FTY_METRIC_COMPOSITE_PRIVATE
#   endif
#endif

//  Project has no stable classes, so we build the draft API
#undef  FTY_METRIC_COMPOSITE_BUILD_DRAFT_API
#define FTY_METRIC_COMPOSITE_BUILD_DRAFT_API

//  Opaque class structures to allow forward references
//  These classes are stable or legacy and built in all releases
//  Draft classes are by default not built in stable releases
#ifdef FTY_METRIC_COMPOSITE_BUILD_DRAFT_API
typedef struct _fty_metric_composite_server_t fty_metric_composite_server_t;
#define FTY_METRIC_COMPOSITE_SERVER_T_DEFINED
typedef struct _fty_metric_composite_configurator_server_t fty_metric_composite_configurator_server_t;
#define FTY_METRIC_COMPOSITE_CONFIGURATOR_SERVER_T_DEFINED
#endif // FTY_METRIC_COMPOSITE_BUILD_DRAFT_API


//  Public classes, each with its own header file
#ifdef FTY_METRIC_COMPOSITE_BUILD_DRAFT_API
#include "fty_metric_composite_server.h"
#include "fty_metric_composite_configurator_server.h"
#endif // FTY_METRIC_COMPOSITE_BUILD_DRAFT_API

#ifdef FTY_METRIC_COMPOSITE_BUILD_DRAFT_API
//  Self test for private classes
FTY_METRIC_COMPOSITE_EXPORT void
    fty_metric_composite_private_selftest (bool verbose);
#endif // FTY_METRIC_COMPOSITE_BUILD_DRAFT_API

#endif
/*
################################################################################
#  THIS FILE IS 100% GENERATED BY ZPROJECT; DO NOT EDIT EXCEPT EXPERIMENTALLY  #
#  Read the zproject/README.md for information about making permanent changes. #
################################################################################
*/
