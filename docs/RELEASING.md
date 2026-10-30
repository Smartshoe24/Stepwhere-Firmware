# Releasing firmware to devices

Pushing a tag `vX.Y.Z` runs `.github/workflows/release.yml`. It builds,
signs, checks, waits for approval, and uploads to AWS. Devices pick it up at
their next daily check.

## One-time setup

### 1. AWS (once)
Deploy `infra/aws/firmware-hosting.yaml` (see `infra/aws/README.md`). Note the
outputs `BucketName`, `DistributionId`, `PublishRoleArn`.

### 2. GitHub environment (once, repo admin, in the browser)
GitHub repo → **Settings → Environments → New environment** → name
`firmware-release`.

* **Required reviewers:** add yourself (and anyone else allowed to ship).
  Every release then waits for a click on "Approve".
* **Deployment branches and tags:** "Selected" → add tag rule `v*`.
* **Environment secret** `OTA_SIGNING_KEY`: open
  `~/stepwhere-keys/ota_signing_key.pem` in TextEdit, copy **all** of it
  including the `-----BEGIN` and `-----END` lines, paste, save.
* **Environment variables:**
  `FIRMWARE_BUCKET` = BucketName, `FIRMWARE_DISTRIBUTION_ID` = DistributionId,
  `AWS_PUBLISH_ROLE_ARN` = PublishRoleArn.

The signing key exists in exactly two places: your offline backup and this
GitHub secret. GitHub never shows it again after saving.

## Making a release

1. Set `#define APP_VERSION "v1.3.0"` in `stepwhere/stepwhere.ino`, merge to main.
2. Tag the merged commit and push the tag:
   ```sh
   git tag v1.3.0
   git push origin v1.3.0
   ```
3. GitHub → **Actions** → "Release firmware" → **Review deployments → Approve**.
4. When it is green: `http://<CloudFrontDomain>/manifest.json` shows 1.3.0.

## What stops a release

| Gate | Fails when |
|---|---|
| 1 | tag differs from `APP_VERSION` |
| 2 | `stepwhere.ino` does not call `ota_meta_confirm()` (every device would roll back) |
| 3 | signed image does not verify with `tools/production_public_key.pem` (wrong secret) |
| 4 | version is not newer than the one published |
| 5 | nobody approves |

Nothing is uploaded unless every gate passes. The image is uploaded before
the manifest, so devices never see a manifest pointing to a missing file.

## Rolling back a bad release

Devices roll back on their own if the new firmware does not confirm. If it
confirms but misbehaves, release a fixed version with a higher number (gate 4
blocks re-publishing an older number on purpose).
