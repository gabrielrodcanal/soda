import collections
import functools
import logging
import operator

from haoda import ir
from haoda import util
from haoda.ir import visitor

_logger = logging.getLogger().getChild(__name__)

def _print_interface(printer, kernel_name, inputs, outputs, super_source):
  """Prints the top-level module for the given arguments.

  Prints the top-level interfaces and sub-module instances with proper interface
  pragmas, hls::stream declarations and references, and module function calls.
  Currently only streaming applications are supported.

  Args:
    printer: CppPrinter to which the code is emitted.
    kernel_name: str, name of the kernel.
    inputs: Sequence of (name, c_type, bank, depth) tuples, specifies the m_axi
      input interfaces.
    outputs: Sequence of (name, c_type, bank, depth) tuples, specifies the m_axi
      output interfaces.
    super_source: SuperSourceNode of a DAG of HAODA nodes.
  """
  println = printer.println
  do_indent = printer.do_indent
  un_indent = printer.un_indent
  do_scope = printer.do_scope
  un_scope = printer.un_scope

  get_bundle_name = util.get_bundle_name
  get_port_name = util.get_port_name
  get_port_buf_name = util.get_port_buf_name

  println('extern "C"')
  println('{')
  println()
  println('void %s(' % kernel_name)
  do_indent()
  for name, c_type, bank, _ in outputs + inputs:
    println('{}* {},'.format(c_type, get_port_name(name, bank)))
  println('uint64_t coalesced_data_num)')
  un_indent()
  do_scope()

  for name, c_type, bank, depth in outputs + inputs:
    println('#pragma HLS interface m_axi port={} offset=slave depth={} bundle={'
            '}'.format(get_port_name(name, bank), depth,
                       get_bundle_name(name, bank)), 0)

  println()
  for name, _, bank, _ in outputs + inputs:
    println('#pragma HLS interface s_axilite port={} bundle=control'.format(
        get_port_name(name, bank)), 0)

  println('#pragma HLS interface s_axilite port=coalesced_data_num '
          'bundle=control', 0)
  println('#pragma HLS interface s_axilite port=return bundle=control', 0)
  println()

  # port buf declarations
  for name, c_type, bank, _ in inputs + outputs:
    println('hls::stream<Data<{c_type}>> {name}("{name}");'.format(
        name=get_port_buf_name(name, bank), c_type=c_type))
  # port buf depths
    println('#pragma HLS stream variable={} depth=32'.format(
        get_port_buf_name(name, bank)), 0)
    println('#pragma HLS data_pack variable={}'.format(
        get_port_buf_name(name, bank)), indent=0)
  println()

  # internal fifos
  for node in super_source.tpo_node_gen():
    for fifo in node.fifos:
      println('hls::stream<Data<{0}>> {1}("{1}");'.format(fifo.c_type,
                                                          fifo.c_expr))
      println('#pragma HLS stream variable={} depth={}'.format(
          fifo.c_expr, fifo.depth+2), 0)
      println('#pragma HLS data_pack variable={}'.format(fifo.c_expr),
              indent=0)

  # pylint: disable=pointless-string-statement
  '''
  if extra_params:
    for param in extra_params.values():
      if param.dup:
        dup = ('[%d]' % param.dup)
      else:
        dup = ''
      println('%s %s%s[UNROLL_FACTOR][%s];' % (param.type, param.name, dup, ']['.join(map(str, param.size))))
    println()

    for param in extra_params.values():
      println('#pragma HLS array_partition variable=%s complete dim=1' % param.name, 0)
      dim_offset = 1
      if param.dup:
        println('#pragma HLS array_partition variable=%s complete dim=2' % param.name, 0)
        dim_offset = 2
      for partitioning in param.partitioning:
        println('#pragma HLS array_partition variable=%s %s dim=%d%s' % (
          param.name,
          partitioning.partition_type,
          dim_offset+1 if partitioning.dim is None else partitioning.dim+dim_offset,
          '' if partitioning.factor is None else ' factor=%d' % partitioning.factor,
        ), 0)
    println()

    for param in extra_params.values():
      if len(param.size) > 1:
        for dim, size in enumerate(param.size):
          println('uint32_t %s_index_dim_%d = 0;' % (param.name, dim))
      println('%s_init:' % param.name, 0)
      println('for(int %s_index = 0; %s_index < %d; ++%s_index)' % (param.name, param.name, functools.reduce(operator.mul, param.size), param.name))
      p.do_scope()
      println('#pragma HLS pipeline II=1', 0)
      println('%s& %s_tmp = var_%s[%s_index];' % (param.type, param.name, param.name, param.name))
      println('%s_unrolled:' % param.name, 0)
      println('for(int unroll_index = 0; unroll_index < UNROLL_FACTOR; ++unroll_index)')
      p.do_scope()
      println('#pragma HLS unroll',0)
      if param.dup is None:
        println('%s[unroll_index]%s = %s_tmp;' % ((param.name, ''.join(['[%s_index_dim_%d]' % (param.name, x) for x in range(len(param.size)-1, -1, -1)]) if len(param.size)>1 else '[%s_index]' % param.name, param.name)))
      else:
        for i in range(param.dup):
          println('%s[%d][unroll_index]%s = %s_tmp;' % (param.name, i, ''.join(['[%s_index_dim_%d]' % (param.name, x) for x in range(len(param.size)-1, -1, -1)]) if len(param.size)>1 else '[%s_index]' % param.name, param.name))
      p.un_scope()
      if len(param.size) > 1:
        for dim in range(len(param.size)):
          println('++%s_index_dim_%d;' % (param.name, dim))
          if dim<len(param.size)-1:
            println('if(%s_index_dim_%d==%d)' % (param.name, dim, param.size[len(param.size)-1-dim]))
            p.do_scope()
            println('%s_index_dim_%d = 0;' % (param.name, dim))
      for size in param.size[:-1]:
        p.un_scope()
      p.un_scope()
    println()

  extra_params_str = ''.join([param.name+', ' for param in extra_params.values()])
  '''

  println()

  # TODO: replication not supported
  # pylint: disable=pointless-string-statement
  '''
  if stencil.replication_factor > 1:
    _generate_code(p, stencil)

    p.un_scope()
    println()
    println('}  //extern "C"')

    return

  # reuse buffers
  if stencil.cluster == 'none':
    for name, reuse_buffer in reuse_buffers.items():
      pragmas = []
      msg = 'reuse buffers for %s' % name
      _logger.debug('generate %s', msg)
      println('// %s' % msg)
      for start, end in reuse_buffer[1:]:
        println('hls::stream<{0}> {1}("{1}");'.format(
            stencil.tensors[name].c_type, get_tensor_name_at(name, end)))
        buffer_length = stencil.reuse_buffer_lengths[name][end]
        tensor_name = get_tensor_name_at(name, end)
        if buffer_length > 1:
          pragmas.append((tensor_name, buffer_length))
      for pragma in pragmas:
        println('#pragma HLS stream variable=%s depth=%d' % pragma, 0)
      println()
  else:
    println('// %s' % stencil.input.name)
    for unroll_index in range(unroll_factor):
      for c in range(stencil.input.chan):
        println('hls::stream<{0}> {1}("{1}");'.format(
            stencil.tensors[stencil.input.name].type,
            get_tensor_name_at(stencil.input.name, unroll_index, c)))
    println()

  for output_name in stencil.output_names:
    println('// %s' % output_name)
    for unroll_index in range(unroll_factor):
      println('hls::stream<{0}> {1}("{1}");'.format(
          stencil.tensors[output_name].c_type,
          get_tensor_name_at(output_name, unroll_index)))
  println()

  # params
  msg = 'params'
  _logger.debug('generate %s' % msg)
  println('// %s' % msg)
  pragmas = []
  for tensor in stencil.chronological_tensors:
    for pe_id in range(unroll_factor):
      for input_name, input_window in tensor.ld_indices.items():
        for param_id in range(len(input_window)):
          offset = next(offset for offset, points in
            all_points[input_name][tensor.name].items()
            if pe_id in points and points[pe_id] == param_id)
          fwd_node = super_source.fwd_nodes[(input_name, offset)]
          cpt_node = super_source.cpt_nodes[(tensor.name, pe_id)]
          extra_depth = super_source.get_extra_depth(
            (fwd_node, cpt_node))
          var_type = stencil.tensors[input_name].c_type
          var_name = 'from_%s_to_%s_param_%d_pe_%d' % (
            input_name, tensor.name, param_id, pe_id)
          println('hls::stream<{0}> {1}("{1}");'.format(var_type, var_name))
          if extra_depth > 0:
            pragmas.append((var_name, extra_depth+1))
          else:
            pragmas.append((var_name, 2))
  for pragma in pragmas:
    println('#pragma HLS stream variable=%s depth=%d' % pragma, 0)
  println()
  '''

  '''
  # border buffers
  msg = 'border buffers'
  _logger.debug('generate %s' % msg)
  println('// %s' % msg)
  for tensor in stencil.chronological_tensors:
    if tensor.output.preserve_border_to():
      for unroll_index in range(unroll_factor):
        for d in range(stencil.dim-1):
          for c in range(tensor.output.chan):
            println('hls::stream<{0}> {1}("{1}");'.foramt(
                tensor.output.type,
                'border_from_%s_dim_%d_left_chan_%d_pe_%d' % (
                    tensor.name, d, c, unroll_index)))
            println('hls::stream<{0}> {1}("{1}");'.foramt(
                tensor.output.type,
                'border_from_%s_dim_%d_right_chan_%d_pe_%d' % (
                    tensor.name, d, c, unroll_index)))
  println()
  '''

  println('#pragma HLS dataflow', 0)
  for name, _, bank, _ in inputs:
    println('BurstRead(&{}, {}, coalesced_data_num);'.format(
        get_port_buf_name(name, bank), get_port_name(name, bank)))

  for node in super_source.tpo_node_gen():
    module_trait_id = super_source.module_table[node][1]
    _print_module_func_call(printer, node, module_trait_id)

  for name, _, bank, _ in outputs:
    println('BurstWrite({}, &{}, coalesced_data_num);'.format(
        get_port_name(name, bank), get_port_buf_name(name, bank)))

  un_scope()
  println()
  println('}//extern "C"')

def print_header(printer):
  println = printer.println
  for header in ['float', 'math', 'stdbool', 'stddef', 'stdint', 'stdio',
                 'string', 'ap_int', 'hls_stream']:
    println('#include<%s.h>' % header)
  println()
  for header in ['algorithm']:
    println('#include<%s>' % header)
  println()

def _print_burst_read(printer):
  println = printer.println
  do_scope = printer.do_scope
  un_scope = printer.un_scope
  println('void BurstRead(hls::stream<Data<ap_uint<BURST_WIDTH>>>* to, ap_uint<'
          'BURST_WIDTH>* from, uint64_t data_num)')
  do_scope()
  println('load_epoch:', 0)
  println('for (uint64_t epoch = 0; epoch < data_num;)')
  do_scope()
  println('#pragma HLS pipeline II=1', 0)
  println('const uint64_t next_epoch = epoch + 1;')
  println('WriteData(to, from[epoch], next_epoch < data_num);')
  println('epoch = next_epoch;')
  un_scope()
  un_scope()

def _print_burst_write(printer):
  println = printer.println
  do_scope = printer.do_scope
  un_scope = printer.un_scope
  println('void BurstWrite(ap_uint<BURST_WIDTH>* to, hls::stream<Data<ap_uint<B'
          'URST_WIDTH>>>* from, uint64_t data_num)')
  do_scope()
  println('store_epoch:', 0)
  println('for (uint64_t epoch = 0; epoch < data_num; ++epoch)')
  do_scope()
  println('#pragma HLS pipeline II=1', 0)
  println('ap_uint<BURST_WIDTH> buf;')
  println('ReadData(&buf, from);')
  println('to[epoch] = buf;')
  un_scope()
  un_scope()

def print_code(stencil, output_file):
  _logger.info('generate kernel code as %s' % output_file.name)
  printer = util.CppPrinter(output_file)

  print_header(printer)

  printer.println()

  util.print_define(printer, 'BURST_WIDTH', stencil.burst_width)
  printer.println()

  util.print_guard(printer, 'UNROLL_FACTOR', stencil.unroll_factor)
  for i in range(len(stencil.tile_size)-1):
    util.print_guard(printer, 'TILE_SIZE_DIM_%d' % i, stencil.tile_size[i])
  util.print_guard(printer, 'BURST_WIDTH', stencil.burst_width)
  printer.println()

  _print_data_struct(printer)
  _print_reinterpret(printer)
  _print_read_data(printer)
  _print_write_data(printer)

  _print_burst_read(printer)
  _print_burst_write(printer)

  for module_trait_id, module_trait in enumerate(stencil.module_traits):
    print_module_definition(printer, module_trait, module_trait_id,
                           burst_width=stencil.burst_width)


  # pylint: disable=pointless-string-statement
  '''
  if stencil.cluster == 'none':
    for forwarder in stencil.get_forwarders():
      print_forward_func(printer, forwarder)
      printer.println()

    for forwarder in stencil.get_forwarders_with_border():
      print_forward_func_with_border(printer, stencil, forwarder)
      printer.println()
    for stage in stencil.stages.values():
      print_compute_stage(printer, stencil, stage)
      printer.println()
  elif stencil.cluster == 'fine':
    print_compute_input(printer, stencil)
    for stage in stencil.get_stages_chronologically():
      print_compute_stage(printer, stencil, stage)

  if stencil.replication_factor > 1:
    dst_lists = set()
    super_source = stencil.dataflow_super_source
    def add_dst_lists(tensor):
      rf = stencil.replication_factor
      dst_ids = [start%rf for start, end
        in stencil.get_replicated_reuse_buffers()[tensor.name][1:]
        if start == end]
      dst_ids = tuple(dst_id if dst_id in dst_ids else None
        for dst_id in range(stencil.replication_factor))
      dst_lists.add(dst_ids)
    for node in super_source.tpo_node_generator():
      if isinstance(node, SuperSourceNode):
        add_dst_lists(stencil.input)
      elif isinstance(node, ComputeNode):
        if not node.stage.is_output():
          add_dst_lists(node.stage.output)
    for dst_list in dst_lists:
      _print_reconnect_func(printer, dst_list)
  printer.println()
  '''

  outputs = []
  inputs = []
  for stmt in stencil.output_stmts:
    for bank in sorted(stmt.dram):
      outputs.append((stmt.name, 'ap_uint<%d>' % stencil.burst_width, bank,
                      65536))
  for stmt in stencil.input_stmts:
    for bank in sorted(stmt.dram):
      inputs.append((stmt.name, 'ap_uint<%d>' % stencil.burst_width, bank,
                     65536))
  for stmt in stencil.param_stmts:
    inputs.append(('var_%s' % stmt.name, stmt.type, 0,
                    functools.reduce(operator.mul, stmt.size)))
  _print_interface(printer, stencil.app_name + '_kernel', inputs, outputs,
                   stencil.dataflow_super_source)

def _print_module_func_call(printer, node, module_trait_id, **kwargs):
  println = printer.println
  print_func = printer.print_func
  func_name = util.get_func_name(module_trait_id)

  dram_reads = tuple(
      '/* input*/ &' + util.get_port_buf_name(dram_ref.var, bank)
      for dram_ref, bank in node.dram_reads)
  dram_writes = tuple(
      '/*output*/ &' + util.get_port_buf_name(dram_ref.var, bank)
      for dram_ref, bank in node.dram_writes)
  output_fifos = tuple('/*output*/ &' + _ for _ in node.output_fifos)
  input_fifos = tuple('/* input*/ &' + _ for _ in node.input_fifos)
  params = dram_writes + output_fifos + input_fifos + dram_reads

  print_func(func_name, params, suffix=';', align=0)

# pylint: disable=too-many-branches,too-many-statements
def print_module_definition(printer, module_trait, module_trait_id, **kwargs):
  println = printer.println
  do_scope = printer.do_scope
  un_scope = printer.un_scope
  func_name = util.get_func_name(module_trait_id)
  func_lower_name = util.get_module_name(module_trait_id)
  ii = 1

  def get_delays(obj, delays):
    if isinstance(obj, ir.DelayedRef):
      delays.append(obj)
    return obj
  delays = []
  for let in module_trait.lets:
    let.visit(get_delays, delays)
  for expr in module_trait.exprs:
    expr.visit(get_delays, delays)
  _logger.debug('delays: %s', delays)

  fifo_loads = tuple('/* input*/ hls::stream<Data<{}>>* {}'.format(
      _.c_type, _.ld_name) for _ in module_trait.loads)
  fifo_stores = tuple('/*output*/ hls::stream<Data<{}>>* {}{}'.format(
      expr.c_type, ir.FIFORef.ST_PREFIX, idx)
    for idx, expr in enumerate(module_trait.exprs))

  # look for DRAM access
  reads_in_lets = tuple(_.expr for _ in module_trait.lets)
  writes_in_lets = tuple(_.name for _ in module_trait.lets
                         if not isinstance(_.name, str))
  reads_in_exprs = module_trait.exprs
  dram_reads = visitor.get_dram_refs(reads_in_lets + reads_in_exprs)
  dram_writes = visitor.get_dram_refs(writes_in_lets)
  dram_read_map = collections.OrderedDict()
  dram_write_map = collections.OrderedDict()
  all_dram_reads = ()
  num_bank_map = {}
  if dram_reads:  # this is an unpacking module
    assert not dram_writes, 'cannot read and write DRAM in the same module'
    for dram_read in dram_reads:
      dram_read_map.setdefault(dram_read.var,
                               collections.OrderedDict()).setdefault(
                                   dram_read.dram, []).append(dram_read)
    _logger.debug('dram read map: %s', dram_read_map)
    burst_width = kwargs.pop('burst_width')
    for var in dram_read_map:
      for dram in dram_read_map[var]:
        # number of elements per cycle
        batch_size = len(dram_read_map[var][dram])
        dram_read_map[var][dram] = collections.OrderedDict(
            (_.offset, _) for _ in dram_read_map[var][dram])
        dram_reads = dram_read_map[var][dram]
        num_banks = len(next(iter(dram_reads.values())).dram)
        if var in num_bank_map:
          assert num_bank_map[var] == num_banks, 'inconsistent num banks'
        else:
          num_bank_map[var] = num_banks
        _logger.debug('dram reads: %s', dram_reads)
        assert tuple(sorted(dram_reads.keys())) == tuple(range(batch_size)), \
               'unexpected DRAM accesses pattern %s' % dram_reads
        batch_width = sum(_.haoda_type.width_in_bits
                          for _ in dram_reads.values())
        del dram_reads
        if burst_width * num_banks >= batch_width:
          assert burst_width * num_banks % batch_width == 0, \
              'cannot process such a burst'
          # a single burst consumed in multiple cycles
          coalescing_factor = burst_width * num_banks // batch_width
          ii = coalescing_factor
        else:
          assert batch_width * num_banks % burst_width == 0, \
              'cannot process such a burst'
          # multiple bursts consumed in a single cycle
          # reassemble_factor = batch_width // (burst_width * num_banks)
          raise util.InternalError('cannot process such a burst yet')
      dram_reads = tuple(next(iter(_.values()))
                         for _ in dram_read_map[var].values())
      all_dram_reads += dram_reads
      fifo_loads += tuple(
          '/* input*/ hls::stream<Data<ap_uint<{burst_width}>>>* '
          '{bank_name}'.format(
              burst_width=burst_width, bank_name=_.dram_fifo_name(bank))
          for _ in dram_reads for bank in _.dram)
  elif dram_writes:   # this is a packing module
    for dram_write in dram_writes:
      dram_write_map.setdefault(dram_write.var,
                                collections.OrderedDict()).setdefault(
                                    dram_write.dram, []).append(dram_write)
    _logger.debug('dram write map: %s', dram_write_map)
    burst_width = kwargs.pop('burst_width')
    for var in dram_write_map:
      for dram in dram_write_map[var]:
        # number of elements per cycle
        batch_size = len(dram_write_map[var][dram])
        dram_write_map[var][dram] = collections.OrderedDict(
            (_.offset, _) for _ in dram_write_map[var][dram])
        dram_writes = dram_write_map[var][dram]
        num_banks = len(next(iter(dram_writes.values())).dram)
        if var in num_bank_map:
          assert num_bank_map[var] == num_banks, 'inconsistent num banks'
        else:
          num_bank_map[var] = num_banks
        _logger.debug('dram writes: %s', dram_writes)
        assert tuple(sorted(dram_writes.keys())) == tuple(range(batch_size)), \
               'unexpected DRAM accesses pattern %s' % dram_writes
        batch_width = sum(_.haoda_type.width_in_bits
                          for _ in dram_writes.values())
        del dram_writes
        if burst_width * num_banks >= batch_width:
          assert burst_width * num_banks % batch_width == 0, \
              'cannot process such a burst'
          # a single burst consumed in multiple cycles
          coalescing_factor = burst_width * num_banks // batch_width
          ii = coalescing_factor
        else:
          assert batch_width * num_banks % burst_width == 0, \
              'cannot process such a burst'
          # multiple bursts consumed in a single cycle
          # reassemble_factor = batch_width // (burst_width * num_banks)
          raise util.InternalError('cannot process such a burst yet')
      dram_writes = tuple(next(iter(_.values()))
                          for _ in dram_write_map[var].values())
      fifo_stores += tuple(
          '/*output*/ hls::stream<Data<ap_uint<{burst_width}>>>* '
          '{bank_name}'.format(
              burst_width=burst_width, bank_name=_.dram_fifo_name(bank))
          for _ in dram_writes for bank in _.dram)

  # print function
  printer.print_func('void {func_name}'.format(**locals()),
                     fifo_stores+fifo_loads, align=0)
  do_scope(func_name)

  for dram_ref, bank in module_trait.dram_writes:
    println('#pragma HLS data_pack variable = {}'.format(
        dram_ref.dram_fifo_name(bank)), 0)
  for arg in module_trait.output_fifos:
    println('#pragma HLS data_pack variable = %s' % arg, 0)
  for arg in module_trait.input_fifos:
    println('#pragma HLS data_pack variable = %s' % arg, 0)
  for dram_ref, bank in module_trait.dram_reads:
    println('#pragma HLS data_pack variable = {}'.format(
        dram_ref.dram_fifo_name(bank)), 0)

  # print inter-iteration declarations
  for delay in delays:
    println(delay.c_buf_decl)
    println(delay.c_ptr_decl)

  # print loop
  println('{}_epoch:'.format(func_lower_name), indent=0)
  println('for (bool enable = true; enable;)')
  do_scope('for {}_epoch'.format(func_lower_name))
  println('#pragma HLS pipeline II=%d' % ii, 0)
  for delay in delays:
    println('#pragma HLS dependence variable=%s inter false' %
            delay.buf_name, 0)

  # print emptyness tests
  println('if (%s)' % (' && '.join(
      '!{fifo}->empty()'.format(fifo=fifo)
      for fifo in tuple(_.ld_name for _ in module_trait.loads) +
                  tuple(_.dram_fifo_name(bank)
                        for _ in all_dram_reads for bank in _.dram))))
  do_scope('if not empty')

  # print intra-iteration declarations
  for fifo_in in module_trait.loads:
    println('{fifo_in.c_type} {fifo_in.ref_name};'.format(**locals()))
  for var in dram_read_map:
    for dram in (next(iter(_.values())) for _ in dram_read_map[var].values()):
      for bank in dram.dram:
        println('ap_uint<{}> {};'.format(burst_width, dram.dram_buf_name(bank)))
  for var in dram_write_map:
    for dram in (next(iter(_.values())) for _ in dram_write_map[var].values()):
      for bank in dram.dram:
        println('ap_uint<{}> {};'.format(burst_width, dram.dram_buf_name(bank)))

  # print enable conditions
  if not dram_write_map:
    for fifo_in in module_trait.loads:
      println('const bool {fifo_in.ref_name}_enable = '
        'ReadData(&{fifo_in.ref_name}, {fifo_in.ld_name});'.format(**locals()))
  for dram in all_dram_reads:
    for bank in dram.dram:
      println('const bool {dram_buf_name}_enable = '
              'ReadData(&{dram_buf_name}, {dram_fifo_name});'.format(
                  dram_buf_name=dram.dram_buf_name(bank),
                  dram_fifo_name=dram.dram_fifo_name(bank)))
  if not dram_write_map:
    println('const bool enabled = %s;' % (
      ' && '.join(tuple('{_.ref_name}_enable'.format(_=_)
                        for _ in module_trait.loads) +
                  tuple('{}_enable'.format(_.dram_buf_name(bank))
                        for _ in all_dram_reads for bank in _.dram))))
    println('enable = enabled;')

  # print delays (if any)
  for delay in delays:
    println('const {} {};'.format(delay.c_type, delay.c_buf_load))

  # print lets
  def mutate_dram_ref_for_writes(obj, kwargs):
    if isinstance(obj, ir.DRAMRef):
      coalescing_idx = kwargs.pop('coalescing_idx')
      unroll_factor = kwargs.pop('unroll_factor')
      type_width = obj.haoda_type.width_in_bits
      elem_idx = coalescing_idx * unroll_factor + obj.offset
      num_banks = num_bank_map[obj.var]
      bank = obj.dram[elem_idx % num_banks]
      lsb = (elem_idx // num_banks) * type_width
      msb = lsb + type_width - 1
      return ir.Var(name='{}({msb}, {lsb})'.format(
          obj.dram_buf_name(bank), msb=msb, lsb=lsb), idx=())
    return obj

  # mutate dram ref for writes
  if dram_write_map:
    for coalescing_idx in range(coalescing_factor):
      for fifo_in in module_trait.loads:
        if coalescing_idx == coalescing_factor - 1:
          prefix = 'const bool {fifo_in.ref_name}_enable = '.format(
              fifo_in=fifo_in)
        else:
          prefix = ''
        println('{prefix}ReadData(&{fifo_in.ref_name},'
                ' {fifo_in.ld_name});'.format(fifo_in=fifo_in, prefix=prefix))
      if coalescing_idx == coalescing_factor - 1:
        println('const bool enabled = %s;' % (
          ' && '.join(tuple('{_.ref_name}_enable'.format(_=_)
                            for _ in module_trait.loads) +
                      tuple('{}_enable'.format(_.dram_buf_name(bank))
                            for _ in dram_reads for bank in _.dram))))
        println('enable = enabled;')
      for idx, let in enumerate(module_trait.lets):
        let = let.visit(mutate_dram_ref_for_writes, {
            'coalescing_idx': coalescing_idx, 'unroll_factor': len(
                dram_write_map[let.name.var][let.name.dram])})
        println('{} = Reinterpret<ap_uint<{width}>>({});'.format(
            let.name, let.expr.c_expr,
            width=let.expr.haoda_type.width_in_bits))
    for var in dram_write_map:
      for dram in (next(iter(_.values()))
                   for _ in dram_write_map[var].values()):
        for bank in dram.dram:
          println('WriteData({}, {}, enabled);'.format(
              dram.dram_fifo_name(bank), dram.dram_buf_name(bank)))
  else:
    for let in module_trait.lets:
      println(let.c_expr)

  def mutate_dram_ref_for_reads(obj, kwargs):
    if isinstance(obj, ir.DRAMRef):
      coalescing_idx = kwargs.pop('coalescing_idx')
      unroll_factor = kwargs.pop('unroll_factor')
      type_width = obj.haoda_type.width_in_bits
      elem_idx = coalescing_idx * unroll_factor + obj.offset
      num_banks = num_bank_map[obj.var]
      bank = expr.dram[elem_idx % num_banks]
      lsb = (elem_idx // num_banks) * type_width
      msb = lsb + type_width - 1
      return ir.Var(
          name='Reinterpret<{c_type}>(static_cast<ap_uint<{width}>>('
               '{dram_buf_name}({msb}, {lsb})))'.format(
                   c_type=obj.c_type, dram_buf_name=obj.dram_buf_name(bank),
                   msb=msb, lsb=lsb, width=msb-lsb+1), idx=())
    return obj

  # mutate dram ref for reads
  if dram_read_map:
    for coalescing_idx in range(coalescing_factor):
      for idx, expr in enumerate(module_trait.exprs):
        println('WriteData({}{}, {}, {});'.format(
            ir.FIFORef.ST_PREFIX, idx,
            expr.visit(mutate_dram_ref_for_reads, {
                'coalescing_idx': coalescing_idx, 'unroll_factor': len(
                    dram_read_map[expr.var][expr.dram])}).c_expr,
            'true' if coalescing_idx < coalescing_factor - 1 else 'enabled'))
  else:
    for idx, expr in enumerate(module_trait.exprs):
      println('WriteData({}{}, {}({}), enabled);'.format(
              ir.FIFORef.ST_PREFIX, idx, expr.c_type, expr.c_expr))

  for delay in delays:
    println(delay.c_buf_store)
    println('{} = {};'.format(delay.ptr, delay.c_next_ptr_expr))

  un_scope()
  un_scope()
  un_scope()
  _logger.debug('printing: %s', module_trait)

def _print_data_struct(printer):
  println = printer.println
  println('template<typename T> struct Data')
  printer.do_scope()
  println('T data;')
  println('bool ctrl;')
  printer.un_scope(suffix=';')

def _print_reinterpret(printer):
  println = printer.println
  println('template<typename To, typename From>')
  println('inline To Reinterpret(From val)')
  printer.do_scope()
  println('#pragma HLS inline', indent=0)
  println('return reinterpret_cast<To&>(val);')
  printer.un_scope()

def _print_read_data(printer):
  println = printer.println
  println('template<typename T> inline bool ReadData'
          '(T* data, hls::stream<Data<T>>* from)')
  printer.do_scope()
  println('#pragma HLS inline', indent=0)
  println('const Data<T>& tmp = from->read();')
  println('*data = tmp.data;')
  println('return tmp.ctrl;')
  printer.un_scope()

def _print_write_data(printer):
  println = printer.println
  println('template<typename T> inline void WriteData'
          '(hls::stream<Data<T>>* to, const T& data, bool ctrl)')
  printer.do_scope()
  println('#pragma HLS inline', indent=0)
  println('Data<T> tmp;')
  println('tmp.data = data;')
  println('tmp.ctrl = ctrl;')
  println('to->write(tmp);')
  printer.un_scope()
