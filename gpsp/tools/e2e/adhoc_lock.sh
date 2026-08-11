# adhoc_lock.sh — serialise every test that starts a PPSSPP ad-hoc instance.
#
# Source this (do not execute it) near the top of any harness script that
# launches two instances and expects them to find each other:
#
#     . "$SCRIPT_DIR/adhoc_lock.sh"; adhoc_lock
#
# WHY THIS EXISTS.  SANDBOX_ROOT isolates the memory sticks and the process
# names, and that is genuinely all it isolates.  Two things stay global no
# matter how many sandboxes exist:
#
#   * PPSSPP's AdhocServer port (27312) is compile-time and binds INADDR_ANY,
#     so the first instance up owns it for the whole machine;
#   * /dev/shm/PPSSPP_ID assigns instance numbers globally, and every harness
#     script deletes it at start.
#
# So a second concurrent ad-hoc test does not fail cleanly -- it half-joins the
# wrong session and hangs at wait_partner, which reads exactly like a transport
# regression in our own code.  Two separate agents each lost time to this
# before it was diagnosed, one of them chasing a "regression" that was another
# agent's emulator.
#
# The fix is exclusion, not more isolation: the resource is genuinely global,
# so the tests must take turns.  Non-ad-hoc gates (boot, save, gu_color) do not
# need this and should not take the lock.
#
# NOTE for anyone editing this file: do NOT write `exec 9>"$f" 2>/dev/null`.
# On an exec with no command, EVERY redirection listed is applied to the shell
# permanently -- so that spelling silently sends the whole calling script's
# stderr to /dev/null for the rest of the run.  The first draft did exactly
# that and swallowed its own timeout message.
ADHOC_LOCK_FILE="${ADHOC_LOCK_FILE:-/tmp/gpsp-adhoc-e2e.lock}"
ADHOC_LOCK_WAIT_S="${ADHOC_LOCK_WAIT_S:-2400}"

# INHERITED-FD HAZARD -- why the two wrappers below exist.
#
# flock(2) is owned by the open file description, not by the process, so the
# lock is released only when the LAST copy of fd 9 closes.  A descriptor
# survives exec() unless it is close-on-exec, so every child of a harness
# inherits fd 9 and can hold the rig lock after the harness is gone.
#
# That is harmless for children that die with the test, but xvfb-run starts
# `Xvfb ... -noreset`, and the call sites all look like
#     timeout N xvfb-run -a -s "..." ...
# so when `timeout` fires it kills xvfb-run, NOT the Xvfb it forked.  The Xvfb
# is reparented to init, -noreset means it never exits on its own, and it keeps
# its inherited copy of fd 9 open forever.  The rig lock is then held by a
# process that is not a test and does not appear in any harness's process tree:
# every later ad-hoc test sits at "waiting for the ad-hoc rig" for the full
# 2400 s with no visible owner.  Clearing the lock FILE does not help either --
# flock follows the inode, and the orphan still pins it.  (97 such orphaned
# Xvfb were found accumulated on this machine before this was fixed.)
#
# Bash cannot set FD_CLOEXEC on a descriptor, and `exec {var}>file` is NOT
# close-on-exec either (verified on bash 5.3), so the descriptor has to be
# closed per command instead.  `9>&-` on an fd that was never opened is a
# no-op, so these are also safe in scripts that never call adhoc_lock.
#
# `timeout` is wrapped as well as xvfb-run because `timeout` is an external
# binary: with `timeout N xvfb-run ...` the shell execs timeout, which execs
# the real /usr/bin/xvfb-run -- an xvfb-run function alone would be bypassed.
timeout()  { command timeout  "$@" 9>&-; }
xvfb-run() { command xvfb-run "$@" 9>&-; }

adhoc_lock() {
  if ! command -v flock >/dev/null 2>&1; then
    echo "adhoc_lock: flock(1) not present — running UNSERIALISED" >&2
    return 0
  fi
  if ! ( umask 000; : >>"$ADHOC_LOCK_FILE" ) 2>/dev/null; then
    echo "adhoc_lock: cannot write $ADHOC_LOCK_FILE — running UNSERIALISED" >&2
    return 0
  fi

  exec 9>>"$ADHOC_LOCK_FILE"

  echo "adhoc_lock: waiting for the ad-hoc rig (max ${ADHOC_LOCK_WAIT_S}s)..."
  if flock -w "$ADHOC_LOCK_WAIT_S" 9; then
    echo "adhoc_lock: acquired (pid $$)"
    return 0
  fi

  echo "adhoc_lock: TIMED OUT after ${ADHOC_LOCK_WAIT_S}s — another ad-hoc test" >&2
  echo "            still holds the rig.  Refusing to run rather than produce a" >&2
  echo "            result that would look like a transport regression." >&2
  exit 1
}
