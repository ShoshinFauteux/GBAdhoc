#!/usr/bin/env python3
"""Atomically append already-built, hash-validated jobs to a live campaign."""
import argparse
from pathlib import Path
import perf_loop as loop


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--campaign', required=True, type=Path)
    parser.add_argument('--remove', action='store_true',
                        help='remove pending IDs instead of appending job directories')
    parser.add_argument('jobs', nargs='+')
    args = parser.parse_args()
    queue_path = args.campaign/'queue.json'
    queue = loop.read_json(queue_path)
    indexed = [(name,loop.load_job(args.campaign/name)['id']) for name in queue]
    existing = {job_id for _,job_id in indexed}
    if args.remove:
        requested=set(args.jobs)
        missing=requested-existing
        if missing: raise ValueError('job not queued: '+', '.join(sorted(missing)))
        journal=loop.read_json(args.campaign/'journal.json')
        active=set(journal['dispatched'])
        if requested & active:
            raise ValueError('cannot remove dispatched job: '+', '.join(sorted(requested & active)))
        loop.save_json(queue_path,[name for name,job_id in indexed if job_id not in requested])
        print('Removed pending:',', '.join(args.jobs))
        return
    additions = []
    for value in args.jobs:
        path = args.campaign/'jobs'/value
        job = loop.load_job(path)
        if job['id'] in existing: raise ValueError('job already queued: '+job['id'])
        existing.add(job['id'])
        additions.append(path.relative_to(args.campaign).as_posix())
    loop.save_json(queue_path,queue+additions)
    print('Enqueued:',', '.join(args.jobs))


if __name__ == '__main__': main()
