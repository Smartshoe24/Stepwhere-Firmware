# Firmware hosting on AWS

Creates (template `firmware-hosting.yaml`):

* S3 bucket `stepwhere-firmware-<account-id>`: private, encrypted, versioned
* CloudFront distribution: the only reader of the bucket; HTTP and HTTPS
* IAM role `stepwhere-firmware-publish`: GitHub Actions, version tags only,
  can upload files and refresh the cache. No access keys anywhere.

Stack region: **ap-south-1 (Mumbai)**. CloudFront itself is global.

## 1. Deploy (AWS CloudShell, ~5 minutes)

1. Log in to the AWS console, region **Asia Pacific (Mumbai)** at the top right.
2. Open **CloudShell** (the `>_` icon in the top bar).
3. Run:

```sh
curl -fsSLO https://raw.githubusercontent.com/Smartshoe24/Stepwhere-Firmware/ota/foundation/infra/aws/firmware-hosting.yaml
aws cloudformation deploy --region ap-south-1 \
  --stack-name stepwhere-firmware \
  --template-file firmware-hosting.yaml \
  --capabilities CAPABILITY_NAMED_IAM
aws cloudformation describe-stacks --region ap-south-1 --stack-name stepwhere-firmware \
  --query "Stacks[0].Outputs" --output table
```

If it fails with "Provider with url https://token.actions.githubusercontent.com
already exists", the account already trusts GitHub. Run the deploy again with
`--parameter-overrides CreateGitHubOidcProvider=false`.

CloudFront takes 5–15 minutes to come online.

## 2. Send back the outputs

`BucketName`, `DistributionId`, `CloudFrontDomain` (like `d1abc.cloudfront.net`),
`PublishRoleArn`. None of these are secrets.

## 3. fw.stepwhere.in at GoDaddy (later, optional)

1. GoDaddy → DNS for `stepwhere.in` → add **CNAME** `fw` → `<CloudFrontDomain>`.
2. For HTTPS on that name (browsers only; devices use HTTP): request an ACM
   certificate for `fw.stepwhere.in` in **us-east-1**, add the validation CNAME
   at GoDaddy, then redeploy with
   `--parameter-overrides CustomDomain=fw.stepwhere.in CertificateArn=<arn>`.

Devices only ever use `fw.stepwhere.in`, so the storage behind it can change
without touching firmware.

## Layout in the bucket

```
manifest.json                   newest release, Cache-Control: max-age=60
releases/1.3.0/stepwhere.ota    immutable, Cache-Control: max-age=31536000
```

## Removing

`aws cloudformation delete-stack --stack-name stepwhere-firmware` keeps the
bucket (DeletionPolicy Retain) so released firmware is never lost by accident.
