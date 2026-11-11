def can_build(env, platform):
    env.module_add_dependencies("tetra_model", ["tetra"])
    return env.editor_build


def configure(env):
    pass
