`gen-test-vectors.mjs` writes `../test-vectors.json` and `../vault-fixture.kdbx`
(`npm i @noble/curves@2.4.0 @noble/ciphers@2.4.0 @noble/hashes@2.4.0 kdbxweb@2.1.1`, then `node gen-test-vectors.mjs ..`).
`verify-test-vectors.py` checks them independently with libsodium (`pip install pysodium`).
Don't regenerate the vectors casually: every implementation pins against them.
