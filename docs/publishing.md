# Publishing Courier

## Prerequisites

Both registries upload source from your local directory — the GitHub repo does not need to be public.

### PlatformIO

1. Create account: `pio account register` (or at [registry.platformio.org](https://registry.platformio.org))
2. Log in: `pio account login`

### ESP Component Registry

1. Sign up at [components.espressif.com](https://components.espressif.com) (GitHub OAuth)
2. Go to **Settings > Tokens**, generate an API token
3. `export IDF_COMPONENT_API_TOKEN=<your-token>`
4. Install compote if needed: `get_idf && pip install idf-component-manager`

Custom namespaces (e.g. `inanimate` instead of your username) require manual approval — request at components.espressif.com before publishing.

## Publishing

### Before publishing

1. Bump `version` in both `library.json` and `idf_component.yml`
2. Verify packaged contents:

```bash
# PlatformIO — shows what files would be included
pio pkg pack

# ESP Component Registry
compote component pack --name courier
```

### PlatformIO Registry

```bash
pio pkg publish --owner inanimate
```

### ESP Component Registry

```bash
compote component upload --name courier --namespace inanimate
```

Dry run (validates without publishing):
```bash
compote component upload --name courier --namespace inanimate --dry-run
```

## Version rules

- **Versions are permanent** — once published, a version can never be reused (even if unpublished)
- **Bump for every release** — both registries reject duplicate versions
- **Use semver** — `0.1.0`, `0.2.0`, `1.0.0`, etc.

## Updating

Same commands as first publish. Just bump the version first.

## CI/CD

For automated publishing (e.g. on GitHub tag):

**PlatformIO:**
```bash
export PLATFORMIO_AUTH_TOKEN=<token>
pio pkg publish --non-interactive
```

**ESP Component Registry:**
```bash
export IDF_COMPONENT_API_TOKEN=<token>
compote component upload --name courier --namespace inanimate
```

Or use Espressif's GitHub Action: [upload-components-ci-action](https://github.com/espressif/upload-components-ci-action).
