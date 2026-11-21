# OpenAI v1.0 Scope Decision: Subscription-First Tracking

> **Status**: Updated planning decision
> **Purpose**: Define the correct OpenAI v1.0 scope now that subscription access and API access are known to be separate surfaces
> **Impact**: Provider requirements, mirror behavior, storage labeling, and blocker definition

## The Question

When JiStats supports OpenAI in v1.0, should the default OpenAI path be:

- **Option A: Subscription-First Local Tracking**
  - Track OpenAI usage from the user's actual subscription-facing workflow
  - Prefer local observation and explicit uncertainty
  - Do not assume API billing or API keys exist

- **Option B: API Import As The Default OpenAI Model**
  - Assume the user has separate API-platform access
  - Build OpenAI around imported organization or project usage
  - Treat API access as the normal source of truth

## What We Now Know

- ChatGPT / Codex subscription access and OpenAI API-platform access are separate products
- Separate billing surfaces mean many valid OpenAI subscribers will not have API billing enabled
- A subscriber may not have an API key at all
- Therefore, OpenAI API import cannot be the default v1.0 assumption if the product goal is subscription usage tracking

## Decision

**Choose Option A: Subscription-First Local Tracking as the default OpenAI v1.0 path.**

**Treat API import as an optional advanced path only when the user independently has OpenAI API-platform access.**

## Why This Is The Correct Decision

1. **It matches the actual user surface**
   - The product goal here is to track subscription usage
   - Many real OpenAI subscribers will not have API access

2. **It avoids a false prerequisite**
   - Requiring API credentials would silently exclude legitimate subscription users
   - That would make the original OpenAI scope narrower than the actual product promise

3. **It keeps the product honest**
   - Subscription-side provider truth may be partially unavailable
   - JiStats should reflect that uncertainty clearly instead of faking exact mirrors

4. **It still leaves room for API import**
   - API-platform data is still valuable
   - It simply becomes a separate, optional capability rather than the default OpenAI identity

## v1.0 OpenAI Scope

### Included

- locally observed OpenAI subscription activity
- local history, trends, and recent activity views
- explicit gap handling and confidence labeling
- truthful `Unknown` state for provider-side quota values that are not yet validated
- storage/model support for an optional future API-import path

### Not Required For Base OpenAI v1.0

- API billing enabled on `platform.openai.com`
- API key ownership
- imported organization-level history
- exact provider-side subscription quota mirroring when no trustworthy signal exists

## Product Behavior Implications

### For a subscription-only OpenAI user

JiStats should:

- show local activity clearly
- preserve local history
- mark provider-side quota state as `Unknown` or `Estimated` where appropriate
- avoid claiming an exact remaining quota or reset time unless validated

### For a user who also has OpenAI API access

JiStats may later:

- import API-platform usage
- show imported cost/history as a separate provider surface
- reconcile or compare local subscription observations with imported API data only when the distinction is clear

## Storage / Modeling Implications

The data model should distinguish:

- `openai_subscription` activity
- `openai_api_import` activity

These should not be merged into a single undifferentiated usage stream unless the UI and analytics make the source explicit.

## Risks

| Risk | Subscription-First Impact |
|------|---------------------------|
| Provider-side subscription quota is not exposed | Show `Unknown`, not a fake mirror |
| Users expect exact quota remaining | Must explain confidence and limitations |
| API-import docs bias implementation | Planning docs must keep the paths separate |
| Future API import adds complexity | Acceptable if modeled as optional and separate |

## Open Questions

1. What trustworthy OpenAI subscription-side signals exist today?
2. Is Codex usage observable in a way that helps subscription tracking locally?
3. How should the UI explain `Unknown` provider quota state without feeling broken?
4. When API import exists, should it appear as a separate account/surface or as a linked enhancement?

## Final Decision

**OpenAI v1.0 default scope = subscription-first local tracking with explicit uncertainty.**

**OpenAI API import = optional later/advanced path, not the default basis of the OpenAI product story.**

## References

- OpenAI mirror spec: `docs/openai-mirror-spec.md`
- Capability matrix: `docs/capability-matrix.md`
- Entity model: `docs/entity-model.md`
