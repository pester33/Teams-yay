-- Gaming Teams v1.1: admin panel (rename, join code, kick, ban, delete) and team goals.
-- Run once in the Supabase SQL Editor after setup.sql. Safe to run again.

-- ----- Admin columns. owner_key and join_code are hidden from everyone (only the functions below read them)
alter table public.team_projects add column if not exists owner_key text not null default '';
alter table public.team_projects add column if not exists join_code text not null default '';
alter table public.team_projects add column if not exists banned text[] not null default '{}';
alter table public.team_projects add column if not exists locked boolean generated always as (join_code <> '') stored;
alter table public.team_members add column if not exists kicked boolean not null default false;

-- Only safe columns can be read or changed directly
revoke all on public.team_projects from anon, authenticated;
grant select (code, host, level_name, snapshot, snapshot_event, saved_by, objects, created_at, updated_at, locked)
  on public.team_projects to anon, authenticated;
grant insert (code, host, level_name, snapshot, snapshot_event, saved_by, objects, owner_key)
  on public.team_projects to anon, authenticated;
grant update (snapshot, snapshot_event, saved_by, objects, updated_at)
  on public.team_projects to anon, authenticated;

revoke all on public.team_members from anon, authenticated;
grant select on public.team_members to anon, authenticated;
grant insert (room, client, name, cursor_x, cursor_y, has_cursor, last_seen) on public.team_members to anon, authenticated;
grant update (room, client, name, cursor_x, cursor_y, has_cursor, last_seen) on public.team_members to anon, authenticated;

-- Admin actions: only work with the level's admin key
create or replace function public.team_admin(p_code text, p_key text, p_action text, p_value text default '')
returns text language plpgsql security definer set search_path = public as $$
declare proj team_projects%rowtype;
begin
  select * into proj from team_projects where code = p_code;
  if not found then return 'missing'; end if;
  if proj.owner_key = '' or proj.owner_key is distinct from p_key then return 'denied'; end if;

  if p_action = 'info' then
    return json_build_object('code', proj.join_code, 'banned', proj.banned)::text;
  elsif p_action = 'rename' then
    if length(trim(p_value)) = 0 or length(p_value) > 40 then return 'bad name'; end if;
    update team_projects set level_name = trim(p_value) where code = p_code;
  elsif p_action = 'set_code' then
    if length(p_value) > 20 then return 'code too long'; end if;
    update team_projects set join_code = p_value where code = p_code;
  elsif p_action = 'kick' then
    update team_members set kicked = true where room = p_code and client = p_value;
  elsif p_action = 'ban' then
    if length(trim(p_value)) = 0 then return 'no name'; end if;
    update team_projects set banned = array_append(array_remove(banned, lower(trim(p_value))), lower(trim(p_value))) where code = p_code;
    update team_members set kicked = true where room = p_code and lower(name) = lower(trim(p_value));
  elsif p_action = 'unban' then
    update team_projects set banned = array_remove(banned, lower(trim(p_value))) where code = p_code;
  elsif p_action = 'delete' then
    delete from team_projects where code = p_code;
  else
    return 'unknown action';
  end if;
  return 'ok';
end;
$$;

-- Before opening a level: banned? needs a join code?
create or replace function public.team_check(p_code text, p_name text, p_join_code text default '')
returns text language plpgsql security definer set search_path = public as $$
declare proj team_projects%rowtype;
begin
  select * into proj from team_projects where code = p_code;
  if not found then return 'missing'; end if;
  if lower(trim(p_name)) = any(proj.banned) then return 'banned'; end if;
  if proj.join_code <> '' and proj.join_code is distinct from p_join_code then
    return case when coalesce(p_join_code, '') = '' then 'code' else 'wrong' end;
  end if;
  return 'ok';
end;
$$;

revoke all on function public.team_admin(text, text, text, text) from public;
revoke all on function public.team_check(text, text, text) from public;
grant execute on function public.team_admin(text, text, text, text) to anon, authenticated;
grant execute on function public.team_check(text, text, text) to anon, authenticated;

-- ----- Team goals: what everyone wants to do on a level
create table if not exists public.team_goals (
  id         bigint generated always as identity primary key,
  room       text not null references public.team_projects(code) on delete cascade,
  author     text not null default '',
  text       text not null check (length(text) between 1 and 120),
  done       boolean not null default false,
  done_by    text not null default '',
  created_at timestamptz not null default now()
);
create index if not exists team_goals_room on public.team_goals (room, id);

alter table public.team_goals enable row level security;
drop policy if exists "goals read"   on public.team_goals;
drop policy if exists "goals add"    on public.team_goals;
drop policy if exists "goals update" on public.team_goals;
create policy "goals read"   on public.team_goals for select using (true);
create policy "goals add"    on public.team_goals for insert with check (true);
create policy "goals update" on public.team_goals for update using (true) with check (true);

revoke all on public.team_goals from anon, authenticated;
grant select on public.team_goals to anon, authenticated;
grant insert (room, author, text) on public.team_goals to anon, authenticated;
grant update (done, done_by) on public.team_goals to anon, authenticated;

-- Removing a goal: whoever wrote it, or the level's admin
create or replace function public.team_goal_remove(p_id bigint, p_name text, p_key text default '')
returns text language plpgsql security definer set search_path = public as $$
declare g team_goals%rowtype; proj team_projects%rowtype;
begin
  select * into g from team_goals where id = p_id;
  if not found then return 'ok'; end if;
  select * into proj from team_projects where code = g.room;
  if lower(g.author) = lower(trim(p_name)) or (proj.owner_key <> '' and proj.owner_key = p_key) then
    delete from team_goals where id = p_id;
    return 'ok';
  end if;
  return 'denied';
end;
$$;
revoke all on function public.team_goal_remove(bigint, text, text) from public;
grant execute on function public.team_goal_remove(bigint, text, text) to anon, authenticated;
