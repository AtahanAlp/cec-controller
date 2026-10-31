#!/usr/bin/env python3
"""Record TV feedback without changing PC power or installing services.

The default only probes and queries status. --power-cycles explicitly requests
standby/wake tests; obtain operator confirmation before using that option.
"""
import argparse
import datetime
import json
from pathlib import Path
import re
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client', type=Path, required=True)
    parser.add_argument('--device', default='/dev/cec-controller')
    parser.add_argument('--physical-address', required=True)
    parser.add_argument('--power-cycles', type=int, choices=range(0, 21), default=0)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    results = []

    def invoke(command):
        start = time.monotonic()
        call = [str(args.client.resolve()), '--config', '/nonexistent', '--device', args.device,
                '--physical-address', args.physical_address, command]
        try:
            result = subprocess.run(call, text=True, capture_output=True, timeout=43)
            output = (result.stdout+result.stderr).strip()
            record = dict(command=command, exit=result.returncode, output=output,
                          fields=dict(re.findall(r'(\w+)=(\S+)', output)))
        except subprocess.TimeoutExpired:
            record = dict(command=command, exit=-1, output='client exceeded the validation deadline', fields={})
        record['elapsed_s'] = round(time.monotonic()-start, 3)
        results.append(record)
        print(json.dumps(record), flush=True)
        return record

    def confirmed(record, state):
        return record['fields'].get('verified') == '1' and record['fields'].get('power') == state

    initial = invoke('protocol')
    if initial['exit'] != 0:
        raise SystemExit('Protocol probe failed; no power tests were performed.')
    invoke('status')
    completed = 0
    routing_verified = True
    last_on = None
    for _ in range(args.power_cycles):
        off = invoke('standby')
        # No fixed delay: the confirmed standby operation itself must serialize wake.
        last_on = invoke('on')
        current = invoke('status')
        routing_verified &= last_on['fields'].get('routing') == 'confirmed'
        if not confirmed(off, 'standby') or not confirmed(last_on, 'on') or not confirmed(current, 'on'):
            break
        completed += 1
    if args.power_cycles and (last_on is None or not confirmed(last_on, 'on')):
        invoke('on')  # bounded restoration attempt, recorded separately from test success
    report = dict(utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  requested_cycles=args.power_cycles, verified_power_cycles=completed,
                  routing_verified=routing_verified if args.power_cycles else None,
                  visible_screen_and_input='requires operator observation', results=results)
    args.report.write_text(json.dumps(report, indent=2)+'\n')
    print('Report: '+str(args.report), flush=True)
    if completed != args.power_cycles:
        return 1
    return 6 if args.power_cycles and not routing_verified else 0


if __name__ == '__main__':
    raise SystemExit(main())
