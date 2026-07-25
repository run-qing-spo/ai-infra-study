# Evidence index

Raw artifacts are ignored by Git. Public documents reference immutable
campaign IDs, harness tree hashes, and selected file hashes.

## Environment

| Artifact | SHA-256 |
|---|---|
| `environment/raw/cloud_runtime_3090_20260725.txt` | `5dd03080dbc8a7b7cac967fd6135854b6c8ad62f79f2368ba05da96eefaa024d` |
| `environment/raw/vllm024_runtime_validation_3090.json` | `0a92d7a5ac22ef7b085e5efe5971054e0c33e81a8123f384ba325647216b7c31` |

SSH host key observed on first connection:
`SHA256:Xe3L/8ja9Qo37KrfvElLR58YTOhv0KiEG8idfv/CEr0` (ED25519).

## Native FS

Campaign `20260725T155651Z-36af5ae1`:

| Artifact | SHA-256 |
|---|---|
| `campaign.json` | `9467e8b0bfa56d743f637b28b79965c896c6d656d0eef4d463de1e61ab240590` |
| `summary.json` | `67a1e8cc86142bfe504b25c8f53146474e6982cd3be944a3ba41ec91d416c2eb` |

Harness tree:
`4d4eb66bc2da73a4e75a89579d53b00cea401cf0f8f6b668bf364e07eb0f7c9c`.
The archived snapshot matches all 11 file hashes in `campaign.json`.

## fio controls

Invalid v1 campaign `20260725T164752Z-efc632e8`:

- `campaign.json`:
  `8088fa27aa9ce0e277e0cedebb4a5197e1f3acdda98def6c0c223a714a2bfcf7`;
- stopped after 20 runs because access seeds differed;
- archived snapshot matches 15/15 manifest hashes;
- excluded from the machine verdict.

Fixed-access v2 campaign `20260725T165904Z-45c3988f`:

| Artifact | SHA-256 |
|---|---|
| `campaign.json` | `66787f31600ea2ee9b19a011a75b64e7249c652e52c28120b6a6a27a1d3c7483` |
| `summary.json` | `b5172bb5189203d681d1d2a77e9ef886d604ebda78fa45e2535af19101b84037` |

Harness tree:
`cfa7488d4ded82622828f4216855c604c7c35a42b8267dc8873b36446ac190a5`.
The archived snapshot matches all 16 file hashes in `campaign.json`.

Interpretation and the frozen A/B success floor:
[Decision 0001](../docs/DECISION_0001_3090_NOISE_GATE.md).

## Long-window paired FS/FS A/A

The fixed 30-pair qualification is defined by
[Decision 0002](../docs/DECISION_0002_UNSTABLE_CLOUD_READ_PATH.md).
No formal campaign has been admitted to the index yet.
