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

#ifndef LIBTARMAC_INDEX_DS_HH
#define LIBTARMAC_INDEX_DS_HH

// This file needs to be included first, as it contains some macro definitions
// to intentionally enable some platform features (e.g. large file support, ...)
// if they have been found by CMake.
#include "libtarmac/platform.hh"

#include "libtarmac/disktree.hh"
#include "libtarmac/misc.hh"

#include <algorithm>

/*

Data structures in the tarmac-browser index file
================================================

This comment lists the trees in the index, and describes what they
store and how they're indexed.

Most of the data structures have the basic form of a balanced binary
search tree (AVL, as it happens), storing an ordered list of items
with respect to some kind of primary sorting criterion.

Annotations
~~~~~~~~~~~

Some of the trees are annotated with additional information at each
node, describing properties of the subtree rooted at that node. This
allows that extra information to be used during search, either to
direct the search, or to make the search compute and return additional
information beyond just finding the target node.

A simple example of a useful tree annotation would be: suppose you
annotated each node of a tree with the total number of elements in the
subtree rooted at that node. Then, given a particular numeric index,
you could find your way to the element at that position (say, the
1234th element of the tree) in a single log-time search: you search
down the tree from the root just as you would if you were looking up
an element by its key, but at each node, instead of comparing the
node's payload against your sorting criterion, you look at the element
count in the node's left child, and that tells you whether the element
you're after is in the left subtree, or the right subtree, or is the
single element in the node itself. Conversely, you could search in the
more usual way, doing a sorting-order comparison at each step, but
*add up* the element counts in the left child of any node where you go
right; then you would find your way to the same node that an
unaugmented search would have found, but you'd also have found out the
number of elements that appear before it, i.e. what its index in the
list is.

To keep the two kinds of data distinct in this document (and the
source code): the term *payload* will be used to describe the contents
of each individual item stored in the tree, and the term *annotation*
will be used to describe information stored about each subtree of the
tree.

Node sharing
~~~~~~~~~~~~

Also, some of the trees are not a single tree, but a large collection
of them all overlapping. These are generated by the indexer repeatedly
saving the current root of the tree (somewhere), and then promising
never to modify any node reachable from that root: further updates to
the tree are done by cloning any 'locked' node it needs to modify, so
that you end up with a new tree root describing the updated state of
the world, but the old tree root and everything reachable from it is
still valid. This is how the system can reconstruct the state of
memory and registers at every point in the trace, for example: there's
a tree root for every observable instant, and each one shares nearly
all its nodes with its predecessor.

The sequential-order tree
~~~~~~~~~~~~~~~~~~~~~~~~~

The top-level tree of the whole index is the sequential order tree,
known as ``seqtree`` in the code. This stores a list of all the events
in the Tarmac trace, sorted by the time at which they occur. Events
that are listed as occurring at the same time (e.g. an instruction
execution together with the register updates and memory accesses it
causes) combine into a single node in this tree.

The payload of ``seqtree`` stores the following data for each event:

 * The time at which the event occurred, as listed explicitly in the
   trace file (in clock cycles, or nanoseconds, or whatever it's
   measuring in).

 * Where to find the event in the trace file, both by range of byte
   positions (for efficient retrieval during browsing) and by range of
   line numbers (for easy correlation to the way other tools will
   understand the file, such as text editors, ``less`` or ``grep``).

 * The value of the program counter when this event takes place. (The
   pc is stored separately from the rest of the register file, because
   Tarmac treats it so differently.)

 * The current call depth, according to the call-tree analysis that
   was done at index generation time. This should go up by 1 at every
   call, and down by 1 at the corresponding return.

 * A pointer to one of the many roots of the memory tree, describing
   the state of registers and memory *after* the event completes.

The tree is sorted by the timestamp, and also by the trace file
position. (They are supposed to be monotonically related, after all!
So the tree can be sorted by both of them at the same time. In fact,
sometimes Tarmac traces list nearby events in the wrong order, but the
indexer deals with that by just coercing too-early timestamps to the
latest time so far seen.)

``seqtree`` also has an annotation for each subtree, whose job is to
allow many searches based on call depth to be done efficiently.

Specifically, the annotation at every tree node stores a cumulative
frequency table, sorted by call depth, each of whose entries is a
tuple storing

 * a call depth, or the value ``SENTINEL_DEPTH`` = ``0xFFFFFFFE``

 * the number of events (``seqtree`` list items) in the whole subtree
   whose call depth is strictly less than the one listed. (The special
   value ``SENTINEL_DEPTH`` indicates that the entry is listing *all*
   the items in the subtree; on the assumption that nothing recurses
   by 2^32 levels without having run out of stack long ago, it
   shouldn't be necessary to actually handle that case specially!)

 * the number of trace file lines in the whole subtree whose call
   depth is strictly less than the one listed

 * cross-links from this entry to the corresponding entries in the
   arrays in this tree node's children. (See below.)

Any missing entries in this tree are implied to have the same value as
the nearest existing entry with a *greater* call depth. For example,
if two successive entries have depths 1 and 10, and cumulative line
counts 100 and 567, then it implies that all depths from 2 to 9
inclusive also have a cumulative line count of 567 - in other words,
there are 467 lines at a call depth of *exactly* 1.

For this reason, the array always starts with a record whose
cumulative counts are equal to zero, and this is not a redundant
entry: it's telling you what range of depths are *not* covered by the
subsequent entry.

(Therefore, the minimal example of one of these arrays, found in leaf
nodes of the sequential order tree, consists of two entries. The first
has a real depth value, and cumulative counts equal to 0; the second
has ``SENTINEL_DEPTH``, a cumulative instruction count of 1, and a
cumulative line count of however many trace lines that one instruction
node covers.)

The cross-links are to the 'corresponding' entry in the sense that, if
the parent entry has depth D, each cross-link field will point at the
index of the first entry in the child array with depth D or greater.
(So they should always point to a real array entry, never past the end
- even if the entry they point to is the one with ``SENTINEL_DEPTH``.)

The intended use of these annotations is to support the Tarmac browser
in folding and unfolding function calls. Suppose you want to hide
everything above call depth 3 (for example), and then you want to be
quickly able to find the right set of lines to display in the window.
You have to be able to answer the question: what is the Nth *visible*
line of the file, subject to the constraint that I'm only interested
in lines at a particular range of call depths?

To answer this kind of question, you start at the root of the tree,
and the first thing you do is a pair of binary searches in the
cumulative frequency table, for the array elements corresponding to
the lowest and highest call depths you care about. Subtracting the
cumulative counts in those array elements tells you how many lines of
the file there are in total, in the depth range of interest.

Then, as you search down the tree, you look up the same information in
each of the arrays at the child nodes you descend to. So if you're
looking for (say) the Nth line in your depth range, you can check
whether the left subtree has at least that many lines; if so, go left;
if not, go right, and decrement N to the index within the right
subtree you're after. (Or stop right here, if you've hit the target
index exactly.)

Conversely, you can use the same array during a search directed by
another criterion, to *discover* the line index of a particular tree
item that you'd found in some other way.

In particular, you can do a search based on one depth range, and
compute a line index as you go based on another depth range. So this
system of annotations lets you ask very complex questions like 'How
many lines of the file in depth range [A,B] occur before the Nth line
in depth range [C,D]?'

The clever part is that this still only takes log time. If you search
down the tree in log N steps, doing a binary search in the array at
each node, it would take (log N)^2 time. But you *don't* need to do
another full binary search in each of those arrays: instead, you can
follow the cross-links from the node you started in, which will get
you to the right entries of the child nodes' arrays in constant time.
So you only need a log-time binary search in the *root* array, and
then you can do the usual log-time search down the tree, with only a
constant amount of extra faffing at each node.

(This technique is called a 'layered range tree'. For more
information, see lecture 3 of the `MIT Advanced Data Structures online
video lecture course
<http://courses.csail.mit.edu/6.851/spring12/lectures/>`_.)

The memory tree (or rather, trees)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Each event in ``seqtree`` contains a root of a memory tree, known as
``memtree`` in the code. This stores the state of registers and memory
just after that event took place, as far as it can be known from the
contents of the trace file.

Once the index is built, all the different ``memtree`` roots can be
treated as if they were independent trees: you search the tree
corresponding to the instant of time you're interested in, and you
don't have to worry about the fact that it shares a lot of its nodes
with the trees before and after it. That's just a space-saving
optimisation.

Registers and memory are stored in the same tree, by pretending that
registers occupy a small address space of their own. So the sorting
key for ``memtree`` is a tuple (address-space identifier, address),
where the address-space identifier is just 'r' for register or 'm' for
memory. If it's 'm', then the address part of the tuple is (obviously)
the actual memory address; if it's 'r', then the address part is a
made-up index value. (This system allows registers to overlap in the
address space, e.g. s0 and d0.)

The payload of a ``memtree`` entry stores the following:

 * The identifier of the address space ('r' or 'm') it describes.

 * An interval of addresses within that address space. (All entries
   reachable from a given ``memtree`` root must have disjoint
   intervals.)

 * One of the following two options:

   * A pointer to the contents of that range of memory, stored as a
     raw block of bytes in the index file.

   * A pointer to a single word in the index file. That word in turn
     stores a pointer to the root of a subsidiary 'memory sub-tree'
     that stores the contents of that memory range as a collection of
     sub-blocks (still sorted by address).

There are two options because the indexer reads the trace file
sequentially during index building, and sometimes it only finds out
later what the contents of some part of memory *was*. So the memory
sub-tree is used when the indexer expects to want to come back later
and fill in the contents.

If the Tarmac trace shows an instruction storing data to memory in the
normal way, the indexer can write out a ``memtree`` entry of the first
kind, with a raw data block containing the values written.

But if the traced program makes a semihosting call, then the data
written into memory by the semihosting implementation - which might be
a *lot* of data, e.g. if you read a big block from a file - is not
shown in full in the trace file. But the indexer can tell what
*region* of memory that semihosting call would have written to; so it
marks the whole region as 'not yet known', by storing a ``memtree``
entry of the second type, with the memory sub-tree starting off empty.
Then, later, when the program *reads* from the memory in that area,
the indexer gets to find out *with hindsight* what data the
semihosting call must have left there. So it writes that into the
memory sub-tree - and now, once the index is made, it can show that
data appearing at the instant of the semihosting call, because that
memory subtree is referred to by *all* the memory representations
between the semihosting call and the time it was later read (and
further into the future as well).

Another even simpler case where the sub-tree approach is used is at
the very start of the trace. We *initialise* ``memtree`` to contain a
single empty memory sub-tree covering the whole address space. Then,
if the program starts reading from portions of memory that were
initialised before tracing began, the browser will be able to show all
the known data in those regions, even at the very start of the trace.

Note that the same memory subtree can be referred to by two or more
entries in the same memtree. This happens if a region of memory is
assigned a new memory subtree, and then an ordinary memory write in
the trace file replaces a piece in the *middle* of it. Then the new
memory tree will have an entry giving the explicit data from that
write, and the previous entry citing the subtree will be divided into
two subintervals, below and above the new write. This is why the
memory subtree has to be indexed by *absolute* address rather than
relative to its base address: it won't necessarily have the same base
address everywhere it's used.

There is also an annotation on each subtree of a ``memtree``, which
stores the following data:

 * The Tarmac trace's explicit timestamp for the most recent event
   that modified any part of the represented range.

This annotation allows the browser to quickly find its way to the
trace event that last changed a piece of memory or a register, so you
can jump back immediately to the instruction that wrote a value you
think is wrong.

Also, the same annotation allows efficiently comparing the same region
of memory between two different times, to highlight all the
sub-regions that changed between those times. (Search the ``memtree``
corresponding to the later time; use the annotations to completely
ignore every subtree that doesn't contain any modification later than
the earlier cutoff time; iterate over whatever is left of the tree
under that filtered view.)

Memory sub-trees
~~~~~~~~~~~~~~~~

For completeness, here's the contents of each memory sub-tree. (These
are all linked from individual nodes of individual memory trees, and
don't share any nodes.)

The payload of a sub-tree entry is very simple:

 * An interval of addresses. (The address-space identifier is not
   needed, because you remember it from the ``memtree`` node that
   linked to the root of this particular sub-tree.)

 * A pointer to a raw block of data in the trace file, of that length,
   storing the data.

As with ``memtree`` itself, all address intervals in the same sub-tree
must be disjoint. The tree is sorted by address. Any address not
belonging to the interval of any of the tree nodes has unknown contents.

The PC tree
~~~~~~~~~~~

In order to allow the browser to easily find visits to a particular
address in the code (say, calls to a given function), there's another
tree, indexing the same events as ``seqtree``, but sorting them by PC.

The payload of a ``bypctree`` entry is:

 * A PC value.

 * The timestamp of a Tarmac trace event at which that PC was visited.

The sorting order is primarily by PC, and secondarily by timestamp. So
you can list all the visits to a function entry point in order, or
find the next or previous one.

The special value 6 in the PC field indicates that a CPU exception
event is happening. This allows browsing tools to locate CPU
exceptions efficiently. 6 cannot be a legal PC value because legal PC
values are all congruent to 0 mod 4 (A32 and A64) or 1 or 3 mod 4
(Thumb, since we use the 'low bit set' representation understood by
BX).

 */

/* ----------------------------------------------------------------------
 * 16-byte magic number at the start of the file that identifies it as
 * a Tarmac Trace Utilities index file. The magic number contains a
 * version, so that an index file can be detected as incompatible with
 * this version of the software.
 *
 * The current reference value of this lives in lib/index_ds.cpp.
 */
struct MagicNumber {
    static const char reference_copy[16 + 1];
    char magic[16];
    void setup();
    bool check();
};

/* ----------------------------------------------------------------------
 * File header that lives immediately after the magic number,
 * containing the roots of other trees.
 */

struct FileHeader {
    diskint<unsigned> flags; // see flag definitions below
    diskint<OFF_T> seqroot;  // root of the sequential order tree
    diskint<OFF_T> bypcroot; // root of the PC tree

    // If the actual Tarmac data starts somewhere other than line 1 of
    // the file (e.g. because of an initial header line), this stores
    // the offset, for adjusting line numbers shown during browsing.
    diskint<unsigned> lineno_offset;
};

// Flag definitions for FileHeader::flags
#define FLAG_BIGEND 0x00000001U // trace was believed big-endian at index time
#define FLAG_AARCH64_USED 0x00000002U // trace includes AArch64 execution state
#define FLAG_COMPLETE 0x00000004U // index generation completed successfully
#define FLAG_THUMB_ONLY 0x00000008U // trace assumes everything is Thumb

/* ----------------------------------------------------------------------
 * Payload and annotation formats for the top-level sequential order tree
 */

struct SeqOrderPayload {
    diskint<Time> mod_time; // timestamp as given in the trace file
    diskint<Addr> pc;       // PC of this node

    // Locations in the trace file, in both bytes and lines
    diskint<OFF_T> trace_file_pos, trace_file_len;
    diskint<unsigned> trace_file_firstline, trace_file_lines;

    // Root of the memory tree representing the state just after this node
    diskint<OFF_T> memory_root;

    // Current depth in the function call hierarchy
    diskint<unsigned> call_depth;

    int cmp(const struct SeqOrderPayload &rhs) const
    {
        if (trace_file_firstline != rhs.trace_file_firstline)
            return trace_file_firstline < rhs.trace_file_firstline ? -1 : +1;
        return 0;
    }
};

struct SeqOrderAnnotation {
    /*
     * This tree annotation sets up the seq-order tree as a layered
     * range tree, i.e. each node is annotated with a whole array of
     * (call depth, cumulative amount of stuff up to that depth)
     * pairs. However, layered range trees are expensive to maintain
     * dynamically, so we don't try: the constructors for this
     * annotation just let all the fields be default-initialised, and
     * we write in the actual arrays in a tree walking pass _after_
     * the whole tree is in its final state.
     */

    // Points to an array of CallDepthArrayEntry structures, as
    // defined below
    diskint<OFF_T> call_depth_array;
    diskint<unsigned> call_depth_arraylen;

    SeqOrderAnnotation() {}
    SeqOrderAnnotation(const SeqOrderPayload &) {}
    SeqOrderAnnotation(const SeqOrderAnnotation &, const SeqOrderAnnotation &)
    {
    }
};

#define SENTINEL_DEPTH (UINT_MAX - 1)
struct CallDepthArrayEntry {
    diskint<unsigned> call_depth;
    diskint<unsigned> cumulative_lines, cumulative_insns;
    diskint<OFF_T> leftlink, rightlink;
};

/* ----------------------------------------------------------------------
 * Payload and annotation formats for the memory tree
 */

struct MemoryPayload {
    char type; // 'r'=register, 'm'=memory

    // If 'raw' is true, then 'contents' is the file offset of an
    // actual sequence of raw bytes representing the memory contents
    // described by this node. If 'raw' is false, then 'contents' is
    // the file offset of a diskint<OFF_T> storing the root of a tree
    // of MemorySubPayload.
    bool raw;

    diskint<Addr> lo, hi; // low and high bytes touched, i.e. inclusive
    diskint<OFF_T> contents;

    // Identifies (by its trace_file_firstline field, i.e. primary
    // key) the seqtree node in which this piece of memory was last
    // touched
    diskint<unsigned> trace_file_firstline;

    int cmp(const struct MemoryPayload &rhs) const
    {
        if (type != rhs.type)
            return type < rhs.type ? -1 : +1;
        if (hi < rhs.lo)
            return -1;
        if (lo > rhs.hi)
            return +1;
        return 0; // any overlap counts as 'equality'
    }
};

struct MemoryAnnotation {
    // Identifies (by its trace_file_firstline field, i.e. primary
    // key) the seqtree node in which any piece of memory within this
    // node's subtree was last touched
    diskint<unsigned> latest;

    MemoryAnnotation() : latest(0) {}
    MemoryAnnotation(const MemoryPayload &p) : latest(p.trace_file_firstline) {}
    MemoryAnnotation(const MemoryAnnotation &lhs, const MemoryAnnotation &rhs)
        : latest(std::max(lhs.latest + 1, rhs.latest + 1) - 1)
    {
    }
};

/* ----------------------------------------------------------------------
 * Payload format for memory subtrees
 */

struct MemorySubPayload {
    diskint<Addr> lo, hi; // low and high bytes touched, i.e. inclusive

    // This is always just a raw range of bytes in the file
    diskint<OFF_T> contents;

    int cmp(const struct MemorySubPayload &rhs) const
    {
        if (hi < rhs.lo)
            return -1;
        if (lo > rhs.hi)
            return +1;
        return 0; // any overlap counts as 'equality'
    }
};

/* ----------------------------------------------------------------------
 * Payload format for the PC tree
 */

struct ByPCPayload {
    diskint<Addr> pc;
    diskint<unsigned> trace_file_firstline;

    int cmp(const struct ByPCPayload &rhs) const
    {
        if (pc != rhs.pc)
            return pc < rhs.pc ? -1 : +1;
        if (trace_file_firstline != rhs.trace_file_firstline)
            return trace_file_firstline < rhs.trace_file_firstline ? -1 : +1;
        return 0;
    }
};

#endif // LIBTARMAC_INDEX_DS_HH
