#!/usr/bin/env python3
# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Read a registered dataset's exact semantics through published OpenAPI."""
import argparse
import json
import urllib.error
import urllib.request


class Client:
    def __init__(self, base):
        self.base = base.rstrip('/') + '/api/semantics/v1'
        with urllib.request.urlopen(self.base + '/openapi.json', timeout=15) as response:
            contract = json.load(response)
        if not contract['openapi'].startswith('3.1.'):
            raise ValueError('OpenAPI 3.1 contract required')
        self.operations = {entry['post']['operationId']: path for path, entry in contract['paths'].items()}

    def call(self, operation_id, payload):
        path = self.operations[operation_id]
        request = urllib.request.Request(self.base + path, data=json.dumps(payload, ensure_ascii=False).encode(),
                                         headers={'Content-Type': 'application/json'})
        try:
            with urllib.request.urlopen(request, timeout=15) as response:
                return json.load(response)['data']
        except urllib.error.HTTPError as error:
            with error:
                details = json.load(error)['error']
            raise RuntimeError(json.dumps({'status': error.code, **details}, ensure_ascii=False)) from None

    def consume(self, provider_id, object_id, field_ids=None):
        binding = self.call('lookupDataSemanticsBinding', {'object_key': {
            'provider_id': provider_id, 'object_id': object_id}})
        binding_snapshot = self.call('resolveDataSemantics', {'exact_reference': binding['exact_reference']})
        selection = {'exact_reference': binding['dictionary_ref'], 'view': 'fields', 'limit': 50}
        if field_ids is not None:
            selection['field_ids'] = field_ids
        fields = self.call('resolveDataSemantics', selection)
        references = self.call('listDataSemanticsReferences', {
            'exact_reference': binding['dictionary_ref'], 'scope': 'history', 'limit': 50})
        # A page remains a projection: preserve next_cursor, gaps and context instead of treating it as a document.
        return {'binding': binding_snapshot, 'dictionary_fields': fields, 'references': references}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base', help='Web origin, for example http://127.0.0.1:8081')
    parser.add_argument('provider_id')
    parser.add_argument('object_id')
    parser.add_argument('--field', action='append', help='field_id; repeat to select multiple fields')
    args = parser.parse_args()
    print(json.dumps(Client(args.base).consume(args.provider_id, args.object_id, args.field),
                     ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
