/*
 ---------------------------------------------------------------------
 Tunefish 4  -  http://tunefish-synth.com
 ---------------------------------------------------------------------
 This file is part of Tunefish.

 Tunefish is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 Tunefish is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with Tunefish.  If not, see <http://www.gnu.org/licenses/>.
 ---------------------------------------------------------------------
 */

#define eVSTI

#include "runtime/system.hpp"
#include "synth/tf4.hpp"
#include "tfsynthprogram.hpp"
#include "factorypatches.hpp"

#include <stdio.h>
#include <string.h>

eTfSynthProgram::eTfSynthProgram()
{

}

eTfSynthProgram::eTfSynthProgram(const eTfSynthProgram& copy) noexcept
    : name(copy.getName())
{
    for (auto i = 0; i < TF_PARAM_COUNT; i++)
        params[i] = copy.getParam(i);
}

eTfSynthProgram& eTfSynthProgram::operator= (const eTfSynthProgram& copy)
{
    name = copy.getName();

    for (auto i = 0; i < TF_PARAM_COUNT; i++)
        params[i] = copy.getParam(i);

    return *this;
}

void eTfSynthProgram::loadFactory(int index)
{
    name = String(TF_FACTORY_PATCH_NAMES[index]);
    for (int i = 0; i < TF_PARAM_COUNT; i++)
        params[i] = static_cast<eF32>(TF_FACTORY_PATCHES[index][i]);
}

void eTfSynthProgram::setParam(eU32 index, eF32 value)
{
    params[index] = value;
}

eF32 eTfSynthProgram::getParam(eU32 index) const
{
    return params[index];
}

String eTfSynthProgram::getName() const
{
    return name;
}

void eTfSynthProgram::setName(String newname)
{
    name = newname;
}

void eTfSynthProgram::loadFromSynth(eTfInstrument* tf)
{
    for (int i = 0; i < TF_PARAM_COUNT; i++)
        setParam(i, tf->params[i]);
}

void eTfSynthProgram::applyToSynth(eTfInstrument* tf) const
{
    for (int i = 0; i < TF_PARAM_COUNT; i++)
        tf->params[i] = getParam(i);
}
