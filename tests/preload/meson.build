# Smoke tests for the LD_PRELOAD/DYLD shim: run an unmodified pthread app
# under the shim with several injected locks and verify (a) the app's own
# correctness counters and (b) that interception actually happened.

preload_victim = executable(
  'preload_victim',
  'preload_victim.c',
  dependencies : [shim_thread_dep],
)

# Victim must pass standalone (no shim) — guards against the test app itself
# being broken.
test('preload_victim_baseline', preload_victim, args : ['4', '5000'], timeout : 60)

preload_runner = find_program('run_preload_test.sh')

foreach lockname : ['mcs', 'ticket', 'exp_spin', 'system']
  test(
    'preload_shim_' + lockname,
    preload_runner,
    args : [
      mutex_preload_shim.full_path(),
      preload_victim.full_path(),
      lockname,
    ],
    depends : [mutex_preload_shim, preload_victim],
    timeout : 180,
  )
endforeach

# Caller-origin filtering: only mutexes first locked from the victim binary's
# own text segment get claimed (they all are, here — the point is the
# /proc/self/maps / dyld text-range plumbing).
test(
  'preload_shim_scope_app',
  preload_runner,
  args : [
    mutex_preload_shim.full_path(),
    preload_victim.full_path(),
    'mcs',
    'app',
  ],
  depends : [mutex_preload_shim, preload_victim],
  timeout : 180,
)
