/*
 * Copyright 2016-2021 Arm Limited. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This file is part of Tarmac Trace Utilities
 */

#include "libtarmac/argparse.hh"
#include "libtarmac/calltree.hh"
#include "libtarmac/intl.hh"
#include "libtarmac/reporter.hh"
#include "libtarmac/tarmacutil.hh"

std::unique_ptr<Reporter> reporter = make_cli_reporter();

int main(int argc, char **argv)
{
    gettext_setup(true);

    IndexerParams iparams;
    iparams.record_memory = false;

    CallTreeOptions ctopts;

    Argparse ap("tarmac-calltree", argc, argv);
    TarmacUtility tu;
    tu.set_indexer_params(iparams);
    tu.add_options(ap);
    ctopts.add_options(ap);
    ap.parse();
    tu.setup();

    IndexNavigator IN(tu.trace, tu.image_filename, tu.load_offset);
    CallTree CT(IN);
    CT.setOptions(ctopts);
    CT.dump();
}
