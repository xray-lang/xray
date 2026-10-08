/* Every Channel Source positive retains expected admission success. */
typedef struct ChannelOwnerCase { const char *name; int64_t runtime_expected; } ChannelOwnerCase;
static const ChannelOwnerCase channel_owner_cases[] = {
    {"channel_handles", 42},
    {"channel_module_types", 0},
    {"channel_constructor_minimal", 42},
    {"channel_nullable_minimal", 0},
};
