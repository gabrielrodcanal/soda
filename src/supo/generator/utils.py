#!/usr/bin/python3.6
from collections import deque, namedtuple
from fractions import Fraction
from functools import reduce
import copy
import json
import logging
import math
import operator
import os
import sys

import supo.grammar

# constants
coords_tiled = 'xyzw'
coords_in_tile = 'ijkl'
coords_in_orig = 'pqrs'
type_width = {'uint8_t':8, 'uint16_t':16, 'uint32_t':32, 'uint64_t':64, 'int8_t':8, 'int16_t':16, 'int32_t':32, 'int64_t':64, 'float':32, 'double':64}
max_dram_bank = 4

_logger = logging.getLogger('__main__').getChild(__name__)

class InternalError(Exception):
    pass

# Buffer.name: str
# Buffer.type: str
# Buffer.chan: int
# Buffer.idx: (int, ...)
# Buffer.parent: Stage
# Buffer.children: {Stage, ...}
# Buffer.offset: int
# Buffer.border: ('preserve', {Stage, ...})
class Buffer(object):
    def __init__(self, node):
        self.name = node.name
        self.type = node.type
        self.chan = node.chan
        if isinstance(node, supo.grammar.Output):
            self.idx = next(iter(node.expr)).idx
            for e in node.expr:
                if e.idx != self.idx:
                    raise InternalError('Normalization went wrong')
        elif isinstance(node, supo.grammar.Intermediate):
            self.idx = next(iter(node.expr)).idx
            for e in node.expr:
                if e.idx != self.idx:
                    raise InternalError('Normalization went wrong')
        else:
            self.idx = None
        self.parent = None
        self.children = set()
        self.offset = 0
        self.border = None

    def __str__(self):
        return '%s(%s)' % (type(self).__name__, ', '.join('%s = %s' % (k, v) for k, v in self.__dict__.items()))

    def PreserveBorderTo(self):
        return self.border[1] if self.border is not None and self.border[0] == 'preserve' else None

# Stage.window: {str: [(int, ...), ...], ...}
# Stage.offset: {str: [int, ...], ...}
# Stage.delay: {str: int, ...}
# Stage.expr: [OutputExpr, ...]
# Stage.inputs: {str: Buffer, ...}
# Stage.output: Buffer
# Stage.border: ('preserve', Buffer)
class Stage(object):
    def __init__(self, **kwargs):
        self.window = kwargs.pop('window')
        self.offset = kwargs.pop('offset')
        self.delay = kwargs.pop('delay', {})
        self.expr = kwargs.pop('expr')
        self.inputs = kwargs.pop('inputs')
        self.output = kwargs.pop('output')
        self.border = None

        # shortcuts
        self.name = self.output.name
        self.idx = self.output.idx

    def __str__(self):
        return '%s(%s)' % (type(self).__name__, ', '.join('%s = %s' % (k, v) for k, v in self.__dict__.items()))

    def PreserveBorderFrom(self):
        return self.border[1] if self.border is not None and self.border[0] == 'preserve' else None

class Stencil(object):
    def __init__(self, **kwargs):
        self.iterate = kwargs.pop('iterate')
        if self.iterate < 1:
            raise SemanticError('cannot iterate %d times' % self.iterate)
        self.border = kwargs.pop('border')
        self.preserve_border = self.border == 'preserve'
        # platform determined
        self.burst_width = kwargs.pop('burst_width')
        self.dram_bank = kwargs.pop('dram_bank')
        # application determined
        self.app_name = kwargs.pop('app_name')
        # parameters can be explored
        self.tile_size = kwargs.pop('tile_size')
        self.unroll_factor = kwargs.pop('unroll_factor')
        self.dram_separate = kwargs.pop('dram_separate')
        if self.dram_separate:
            if self.dram_bank%2 != 0:
                logging.getLogger(__name__).fatal('Number of DRAM banks has to be even when separated')
                sys.exit(-1)
            else:
                self.dram_bank = int(self.dram_bank/2)
        # stage-independent
        self.dim = kwargs.pop('dim')
        self.extra_params = kwargs.pop('extra_params')
        # stage-specific
        input_node = kwargs.pop('input')
        output_node = kwargs.pop('output')

        if self.iterate > 1:
            if input_node.type != output_node.type:
                raise SemanticError('input must have the same type as output if iterate > 1 times, current input has type %s but output has type %s' % (input_node.type, output_node.type))
            if input_node.chan != output_node.chan:
                raise SemanticError('input must have the same number of channel as output if iterate > 1 times, current input has %d chanels but output has %d channels' % (input_node.chan, output_node.chan))
            _logger.debug('pipeline %d iterations of %s -> %s' % (self.iterate, input_node.name, output_node.name))

        intermediates = kwargs.pop('intermediates')
        intermediate_names = set(x.name for x in intermediates)

        new_intermediates = []
        preserved_borders = {}  # {to: {from, ...}, ...}
        NameFromIter = lambda n, i: n+'_iter%d' % i if i > 0 else n
        IntermediateLoadCallBack = lambda n: NameFromIter(n, iteration) if n == input_node.name else NameFromIter(n, iteration) if n in intermediate_names else n
        OutputLoadCallBack = lambda n: NameFromIter(n, iteration-1) if n == input_node.name or n in intermediate_names else n
        for iteration in range(1, self.iterate):
            new_intermediate = supo.grammar.Intermediate(output_node=output_node)
            new_intermediate.MutateLoad(OutputLoadCallBack)
            new_intermediate.MutateStore(lambda n: NameFromIter(input_node.name, iteration))
            if self.preserve_border:
                border_from = NameFromIter(input_node.name, iteration-1)
                new_intermediate.PreserveBorder(border_from)
                preserved_borders.setdefault(new_intermediate.name, set()).add(border_from)
            new_intermediates.append(new_intermediate)
            for intermediate in intermediates:
                new_intermediate = copy.deepcopy(intermediate)
                new_intermediate.MutateLoad(IntermediateLoadCallBack)
                new_intermediate.MutateStore(lambda n: NameFromIter(n, iteration))
                new_intermediates.append(new_intermediate)
        if self.preserve_border:
            border_from = NameFromIter(input_node.name, self.iterate-1)
            output_node.PreserveBorder(border_from)
            preserved_borders.setdefault(output_node.name, set()).add(border_from)
        output_node.MutateLoad(lambda n: NameFromIter(n, self.iterate-1))
        intermediates += new_intermediates

        _logger.debug(input_node)
        _logger.debug(intermediates)
        _logger.debug(output_node)

        self.buffers = {i.name: Buffer(i) for i in intermediates}
        if input_node.name in intermediate_names:
            raise SemanticError('input name conflict with buffer: %s' % self.input.name)
        else:
            self.input = Buffer(input_node)
            self.buffers[self.input.name] = self.input
        if output_node.name in intermediate_names:
            raise SemanticError('output name conflict with buffer: %s' % self.output.name)
        else:
            self.output = Buffer(output_node)
            self.buffers[self.output.name] = self.output

        self.stages = {}
        for intermediate in intermediates:
            child_buffer = self.buffers[intermediate.name]
            parent_buffers = self.GetParentBuffersFor(intermediate)
            window = self.GetWindowFor(intermediate)
            this_stage = Stage(window=window,
                offset={n: SerializeIterative(w, self.tile_size) for n, w in window.items()},
                delay={},
                expr=self.GetExprFor(intermediate),
                inputs=parent_buffers,
                output=child_buffer,
                border=intermediate.border)
            self.stages[intermediate.name] = this_stage
            child_buffer.parent = this_stage
            for b in parent_buffers.values():
                b.children.add(this_stage)

        parent_buffers = self.GetParentBuffersFor(output_node)
        window = self.GetWindowFor(output_node)
        output_stage = Stage(window=window,
            offset={n: SerializeIterative(w, self.tile_size) for n, w in window.items()},
            delay={},
            expr=self.GetExprFor(output_node),
            inputs=parent_buffers,
            output=self.output,
            border=output_node.border)
        self.stages[output_node.name] = output_stage
        self.output.parent = output_stage
        for b in parent_buffers.values():
            b.children.add(output_stage)

        # let buffer/stage remember which stages/buffers to send/recv as borders
        for dst, srcs in preserved_borders.items():
            for src in srcs:
                _logger.debug('border from %s to %s' % (src, dst))
            if self.buffers[src].border is None:
                self.buffers[src].border = ('preserve', set())
            self.buffers[src].border[1].add(self.stages[dst])
            if self.stages[dst].border is None:
                self.stages[dst].border = ('preserve', self.buffers[src])

        # now that we have global knowledge of the buffers we can calculate the offsets of buffers
        _logger.info('calculate buffer offsets')
        processing_queue = deque([self.input.name])
        processed_buffers = {self.input.name}
        self.chronological_buffers = [self.input]
        _logger.debug('buffer %s is at offset %d' % (self.input.name, self.input.offset))
        while len(processing_queue)>0:
            b = self.buffers[processing_queue.popleft()]
            _logger.debug('inspecting buffer %s\'s children' % b.name)
            for s in b.children:
                if {x.name for x in s.inputs.values()} <= processed_buffers and s.name not in processed_buffers:
                    # good, all inputs are processed, can determine offset of current buffer
                    _logger.debug('input%s for buffer %s (i.e. %s) %s processed' % ('' if len(s.inputs)==1 else 's', s.name, ', '.join([x.name for x in s.inputs.values()]), 'is' if len(s.inputs)==1 else 'are'))
                    s.output.offset = max([s.output.offset] + [x.offset+s.offset[x.name][-1] for x in s.inputs.values()])
                    _logger.debug('buffer %s is at offset %d' % (s.name, s.output.offset))
                    for x in s.inputs.values():
                        delay = s.output.offset - (x.offset+s.offset[x.name][-1])
                        if delay>0:
                            _logger.debug('buffer %s arrives at buffer %s at offset %d < %d; add %d delay' % (x.name, s.name, x.offset+s.offset[x.name][-1], s.output.offset, delay))
                        else:
                            _logger.debug('buffer %s arrives at buffer %s at offset %d = %d; good' % (x.name, s.name, x.offset+s.offset[x.name][-1], s.output.offset))
                        s.delay[x.name] = max(delay, 0)
                        _logger.debug('set delay of %s <- %s to %d' % (s.name, x.name, s.delay[x.name]))
                    processing_queue.append(s.name)
                    processed_buffers.add(s.name)
                    self.chronological_buffers.append(s.output)
                else:
                    for bb in s.inputs.values():
                        if bb.name not in processed_buffers:
                            _logger.debug('buffer %s requires buffer %s as an input' % (s.name, bb.name))
                            _logger.debug('but buffer %s isn\'t processed yet' % bb.name)
                            _logger.debug('add %s to scheduling queue' % bb.name)
                            processing_queue.append(bb.name)

        # setup preserving borders, has to be done here because overall window cannot be generated before dependency graph is created
        for node in intermediates+[output_node]:
            if hasattr(node, 'preserve_border'):
                # preserve border from node.preserve_border to node.name
                windows = self.stages[node.name].window
                windows.setdefault(node.preserve_border, list(windows.get(node.preserve_border, set())|{next(iter(node.expr)).idx}))
                stencil_window = GetOverallStencilWindow(self.buffers[node.preserve_border], self.buffers[node.name])
                self.stages[node.name].delay.setdefault(node.preserve_border, GetStencilDistance(stencil_window, self.tile_size)-Serialize(GetStencilWindowOffset(stencil_window), self.tile_size))
                _logger.debug('window for %s is %s' % (node.name, windows))
                self.stages[node.name].inputs.setdefault(node.preserve_border, self.buffers[node.preserve_border])
                self.buffers[node.preserve_border].children.add(self.stages[node.name])

        _logger.debug('buffers: '+str(list(self.buffers.keys())))
        LoadPrinter = lambda node: '%s(%s)' % (node.name, ', '.join(map(str, node.idx))) if node.name in self.extra_params else '%s[%d](%s)' % (node.name, node.chan, ', '.join(map(str, node.idx)))
        StorePrinter = lambda node: '%s[%d](%s)' % (node.name, node.chan, ', '.join(map(str, node.idx)))
        for s in self.stages.values():
            _logger.debug('stage: %s@(%s) <- [%s]' % (s.name, ', '.join(map(str, s.idx)), ', '.join('%s@%s' % (x.name, list(set(s.window[x.name]))) for x in s.inputs.values())))
        for s in self.stages.values():
            for e in s.expr:
                _logger.debug('stage.expr: %s' % e.GetCode(LoadPrinter, StorePrinter))
        for s in self.stages.values():
            for n, w in s.offset.items():
                _logger.debug('stage.offset: %s@%d <- %s@[%s]' % (s.name, Serialize(s.output.idx, self.tile_size), n, ', '.join(map(str, w))))
        for s in self.stages.values():
            for n, d in s.delay.items():
                _logger.debug('stage.delay: %s <- %s delayed %d' % (s.name, n, d))

        # parameters generated from the above parameters
        self.pixel_width_i = type_width[self.input.type]
        self.pixel_width_o = type_width[self.output.type]
        self.input_partition  = self.burst_width/self.pixel_width_i*self.dram_bank/2 if self.burst_width/self.pixel_width_i*self.dram_bank/2 > self.unroll_factor/2 else self.unroll_factor/2
        self.output_partition = self.burst_width/self.pixel_width_o*self.dram_bank/2 if self.burst_width/self.pixel_width_o*self.dram_bank/2 > self.unroll_factor/2 else self.unroll_factor/2

    def GetProducerBuffers(self):
        return [b for b in self.buffers.values() if len(b.children)>0]

    def GetConsumerBuffers(self):
        return [b for b in self.buffers.values() if b.parent is not None]

    def GetStagesChronologically(self):
        return [self.stages[b.name] for b in self.chronological_buffers if b.name in self.stages]

    # return [Buffer, ...]
    def GetParentBuffersFor(self, node):
        return {x: self.buffers[x] for x in {x.name for x in node.GetLoads() if x.name not in self.extra_params}}

    # return {name: [(idx, ...), ...]}
    def GetWindowFor(self, node):
        loads = node.GetLoads() # [Load, ...]
        load_names = {l.name for l in loads if l.name not in self.extra_params}
        windows = {name: sorted({l.idx for l in loads if l.name == name}, key=lambda x: Serialize(x, self.tile_size)) for name in load_names}
        _logger.debug('window for %s is %s' % (node.name, windows))
        return windows

    # return [OutputExpr, ...]
    def GetExprFor(self, node):
        if isinstance(node, supo.grammar.Output):
            return node.expr
        if isinstance(node, supo.grammar.Intermediate):
            return node.expr
        raise SemanticError('cannot get expression for %s' % str(type(node)))

class Printer(object):
    def __init__(self, out):
        self.out = out
        self.indent = 0
        self.assign = 0
        self.comments = []

    def PrintLine(self, line = '', local_indent = -1):
        if local_indent < 0:
            local_indent = self.indent
        if line:
            self.out.write('%s%s\n' % (' '*local_indent*4, line))
        else:
            self.out.write('\n')

    def DoIndent(self):
        self.indent += 1

    def UnIndent(self):
        self.indent -= 1

    def DoScope(self, comment=''):
        self.PrintLine('{')
        self.DoIndent()
        self.comments.append(comment)

    def UnScope(self, comment=''):
        self.UnIndent()
        popped_comment = self.comments.pop()
        if comment:
            self.PrintLine('} // %s' % comment)
        else:
            if popped_comment:
                self.PrintLine('} // %s' % popped_comment)
            else:
                self.PrintLine('}')

    def NewVar(self):
        self.assign += 1
        return self.LastVar()

    def LastVar(self, offset=-1):
        return 'assign_%d' % (self.assign+offset)

def GetCType(supo_type):
    if supo_type in {'uint8', 'uint16', 'uint32', 'uint64', 'int8', 'int16', 'int32', 'int64'}:
        return supo_type+'_t'
    return supo_type

def IsFloat(supo_type):
    return supo_type in {'float', 'double'}

def PrintGuard(printer, var, val):
    printer.PrintLine('#if %s != %d' % (var, val))
    printer.PrintLine('#error %s != %d' % (var, val))
    printer.PrintLine('#endif//%s != %d' % (var, val))

def PrintDefine(printer, var, val):
    printer.PrintLine('#ifndef %s' % var)
    printer.PrintLine('#define %s %d' % (var, val))
    printer.PrintLine('#endif//%s' % var)

def Serialize(vec, tile_size):
    return sum((vec[i]*reduce(operator.mul, tile_size[:i]) for i in range(1, len(tile_size))), next(iter(vec)))

def SerializeIterative(iterative, tile_size):
    return [Serialize(x, tile_size) for x in iterative]

def GetStencilDistance(stencil_window, tile_size):
    return max(SerializeIterative(stencil_window, tile_size))+Serialize(GetStencilWindowOffset(stencil_window), tile_size)

def GetStencilDim(A):
    return [max_index-min_index+1 for max_index, min_index in zip([max([point[dim] for point in A]) for dim in range(len(next(iter(A))))], [min([point[dim] for point in A]) for dim in range(len(next(iter(A))))])]

_overall_stencil_window_cache = {}
def GetOverallStencilWindow(input_buffer, output_buffer):
    # normalize store index to 0
    if (id(input_buffer), id(output_buffer)) in _overall_stencil_window_cache:
        return _overall_stencil_window_cache[(id(input_buffer), id(output_buffer))]
    _logger.debug('get overall stencil window of %s <- %s' % (output_buffer.name, input_buffer.name))
    all_points = set()
    if output_buffer.parent is not None:
        for name, points in output_buffer.parent.window.items():
            if name != input_buffer.name:
                recursive_points = GetOverallStencilWindow(input_buffer, output_buffer.parent.inputs[name])
                all_points |= set.union(*[{tuple(map(lambda a, b, c: a + b - c, p, point, output_buffer.idx)) for p in recursive_points} for point in points])
            else:
                all_points |= set(tuple(map(operator.sub, point, output_buffer.idx)) for point in points)
    _logger.debug('overall stencil window of %s (%s) <- %s is %s (%d points)' % (output_buffer.name, ', '.join(['0']*len(output_buffer.idx)), input_buffer.name, all_points, len(all_points)))
    _overall_stencil_window_cache[(id(input_buffer), id(output_buffer))] = all_points
    return all_points

def GetStencilWindowOffset(stencil_window):
    # only works if window is normalized to store at 0
    return tuple(-min(p[d] for p in stencil_window) for d in range(len(next(iter(stencil_window)))))
