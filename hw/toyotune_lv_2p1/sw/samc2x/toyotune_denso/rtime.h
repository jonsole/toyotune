#ifndef __RTIME_H__
#define __RTIME_H__

#define	Time_Add(t1, t2)	((t1) + (t2))
#define	Time_Sub(t1, t2)	((int32_t) (t1) - (int32_t) (t2))
#define	Time_Eq(t1, t2)		((t1) == (t2))
#define	Time_Ne(t1, t2)		((t1) != (t2))
#define Time_Gt(t1, t2)		(Time_Sub((t1), (t2)) > 0)
#define Time_Ge(t1, t2)		(Time_Sub((t1), (t2)) >= 0)
#define Time_Lt(t1, t2)		(Time_Sub((t1), (t2)) < 0)
#define Time_Le(t1, t2)		(Time_Sub((t1), (t2)) <= 0)

#endif	/* __RTIME_H__ */
